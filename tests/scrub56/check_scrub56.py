#!/usr/bin/env python3
"""Execute production RAID56 batch ownership and completion with fault injection."""
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

def function(source, name):
    pos = source.index(name + "(")
    start = source.rfind("\n", 0, pos) + 1
    brace = source.index("{", pos)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

scrub = read("src/scrub.c")
batch = read("src/scrub-raid56.c", optional=True) or scrub
types = read("src/scrub.h", optional=True)
if "} scrub_context_raid56;" not in types:
    types = scrub
start = types.index("typedef struct {\n    uint8_t* buf;\n    PIRP Irp;")
end = types.index("} scrub_context_raid56;") + len("} scrub_context_raid56;")
(out / "types.inc").write_text(types[start:end] + "\n")
(out / "completion.inc").write_text(function(batch, "scrub_read_completion_raid56"))
(out / "batch.inc").write_text(function(batch, "scrub_chunk_raid56_stripe_run"))
(out / "btrfs.h").write_text(read("src/btrfs.h"))
binary = out / "scrub56_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("scrub56_test.c")), "-o", str(binary)], check=True)
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
