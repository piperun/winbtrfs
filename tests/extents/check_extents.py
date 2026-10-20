#!/usr/bin/env python3
"""Exercise production extent rationalization with reference-update failures."""
import argparse
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision", help="Git revision; omit for working-tree sources")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path, optional=False):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                text=True, capture_output=True)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

source = read("src/extent-ranges.c", optional=True) or read("src/flushthread.c")
end = source.index("} extent_range;") + len("} extent_range;")
start = source.rindex("typedef struct {", 0, end)
marker = source.index("NTSTATUS rationalize_extents(")
brace = source.index("{", marker)
end_function, depth = brace + 1, 1
while depth:
    depth += (source[end_function] == "{") - (source[end_function] == "}")
    end_function += 1
(out / "ranges.inc").write_text(source[start:end] + "\n" + source[marker:end_function] + "\n")
(out / "btrfs.h").write_text(read("src/btrfs.h"))
binary = out / "extents_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("extents_test.c")), "-o", str(binary)], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=5)
        if result.returncode:
            print(f"FAIL {case}: exit {result.returncode}", flush=True)
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: five-second timeout", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
