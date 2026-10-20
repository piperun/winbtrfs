#!/usr/bin/env python3
"""Exercise the production retrieval-pointer query with native output fixtures."""
import argparse
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
    source = repo / path
    return source.read_text() if source.exists() or not optional else ""

source = read("src/retrieval.c", optional=True) or read("src/fsctl.c")
start = source.index("NTSTATUS get_retrieval_pointers(")
brace = source.index("{", start)
end, depth = brace + 1, 1
while depth:
    depth += (source[end] == "{") - (source[end] == "}")
    end += 1
function = source[start:end]
# Translate only the structured cleanup syntax; all query logic is unchanged.
function = function.replace("try {", "{").replace("leave;", "goto cleanup;")
function = function.replace("} finally {", "} cleanup: {")
(out / "query.inc").write_text(function + "\n")
(out / "btrfs.h").write_text(read("src/btrfs.h"))
binary = out / "retrieval_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("retrieval_test.c")), "-o", str(binary)], check=True, timeout=30)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = subprocess.check_output([str(binary), "--list"], text=True, timeout=5).splitlines()
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
