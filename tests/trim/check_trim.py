#!/usr/bin/env python3
"""Exercise production TRIM submission and completion with fault injection."""
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

def function(source, marker):
    if marker not in source and marker.startswith("static "): marker = marker[7:]
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

flush = read("src/flushthread.c") + read("src/trim-mapping.c", optional=True)
trim = read("src/trim.c", optional=True) or flush
end = trim.index("} trim_context;") + len("} trim_context;")
start = trim.index("typedef struct {\n    DEVICE_MANAGE_DATA_SET_ATTRIBUTES* dmdsa;")
(out / "types.inc").write_text(trim[start:end] + "\n")
(out / "completion.inc").write_text(function(trim, "static NTSTATUS __stdcall trim_completion("))
submission = function(trim, "NTSTATUS trim_devices(") if "NTSTATUS trim_devices(" in trim else ""
(out / "submission.inc").write_text(submission)
binary = out / "trim_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("trim_test.c")), "-o", str(binary)], check=True)
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
