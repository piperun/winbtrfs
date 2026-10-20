#!/usr/bin/env python3
"""Run native query regressions against unchanged production function bodies."""
import argparse
import os
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)


def read(path):
    if args.revision:
        return subprocess.check_output(
            ["git", "-C", str(repo), "show", f"{args.revision}:{path}"], text=True, timeout=10)
    return (repo / path).read_text()


def read_optional(path):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                capture_output=True, text=True, timeout=10)
        return result.stdout if result.returncode == 0 else None
    source = repo / path
    return source.read_text() if source.exists() else None


header = read("src/btrfs_drv.h")
begin = header.index("#if defined(_MSC_VER) || defined(__clang__)")
end = header.index("#endif", begin) + len("#endif")
macros = header[begin:end]
checksum = read("src/checksum-info.c")
ranges = read("src/allocated-ranges.c")
range_start = "static NTSTATUS append_range(" if "static NTSTATUS append_range(" in ranges else "NTSTATUS query_ranges("
helpers = []
math_header = read_optional("src/range-math.h")
math_source = read_optional("src/range-math.c")
if math_header is not None and math_source is not None:
    (out / "range-math.h").write_text(math_header)
    (out / "range-math.c").write_text(math_source)
    helpers.append(str(out / "range-math.c"))
math_include = '#include "range-math.h"\n' if helpers else ''
(out / "query.inc").write_text(macros + "\n" + math_include + checksum[checksum.index("static NTSTATUS add_csum_sparse_extents("):]
                               + "\n" + ranges[ranges.index(range_start):])
binary = out / "query_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("query_test.c")), *helpers, "-o", str(binary)], check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0")
cases = subprocess.check_output([str(binary), "--list"], text=True, timeout=5, env=env).splitlines()
failed = 0
for case in cases:
    result = subprocess.run([str(binary), case], timeout=5, env=env)
    if result.returncode:
        print(f"FAIL {case}: exit {result.returncode}", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
