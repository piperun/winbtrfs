#!/usr/bin/env python3
"""Exercise the production open_fcb extent acceptance branch with host types."""
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

def read(path, optional=False):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                capture_output=True, text=True, timeout=10)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

source = read("src/fcb-load.c", optional=True) or read("src/create.c")
start = source.index("if (ed->type == BTRFS_FILE_EXTENT_REG")
end = source.index("\n            ext = ExAllocatePoolWithTag", start)
branch = source[start:end]
helper = ""
marker = "static bool valid_file_extent("
if marker in source:
    begin = source.index(marker)
    brace = source.index("{", begin)
    pos, depth = brace + 1, 1
    while depth:
        depth += (source[pos] == "{") - (source[pos] == "}")
        pos += 1
    helper = source[begin:pos] + "\n"
(out / "extent_helper.inc").write_text(helper)
(out / "extent_branch.inc").write_text(branch + "\n")
(out / "btrfs.h").write_text(read("src/btrfs.h"))
binary = out / "extent_load_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("extent_load_test.c")), "-o", str(binary)],
               check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
for case in cases:
    result = subprocess.run([str(binary), case], env=env, timeout=5)
    if result.returncode:
        print(f"FAIL {case}: exit {result.returncode}", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
