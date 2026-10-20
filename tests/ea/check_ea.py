#!/usr/bin/env python3
"""Execute the production EA parser with native layout and allocation checks."""
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
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"], text=True, capture_output=True)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

source = read("src/file-create.c")
license_header = source[:source.index("#include")]
(out / "parse_ea.inc").write_text(license_header + function(source, "static NTSTATUS file_create_parse_ea("))
operations = read("src/ea.c", optional=True) or read("src/fileinfo.c")
(out / "set_ea.inc").write_text(license_header + function(operations, "NTSTATUS __stdcall drv_set_ea("))
header = read("src/btrfs_drv.h")
end = header.index("} ea_item;") + len("} ea_item;")
start = header.rindex("typedef struct {", 0, end)
constants = header[header.index("static const char lxuid[]"):]
constants = constants[:constants.index("\n", constants.index("static const char lxdev[]"))]
(out / "ea_types.inc").write_text(license_header + header[start:end] + "\n" + constants + "\n")
(out / "btrfs.h").write_text(read("src/btrfs.h"))
binary = out / "ea_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("ea_test.c")), "-o", str(binary)], check=True)
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
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
