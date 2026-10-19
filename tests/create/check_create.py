#!/usr/bin/env python3
"""Run production file/stream creation and reaping with host kernel services."""
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
parser.add_argument("--debug-refs", action="store_true")
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

create, btrfs = read("src/create.c"), read("src/btrfs.c")
license_header = create[:create.index("#include")]
ordinary = read("src/file-create.c", optional=True) or create
marker = "static NTSTATUS file_create2(" if "static NTSTATUS file_create2(" in ordinary else "NTSTATUS file_create2("
(out / "file_create2.inc").write_text(license_header + function(ordinary, marker))
for name, marker in [("create_stream", "static NTSTATUS create_stream("),
                     ("create_fcb", "fcb* create_fcb("),
                     ("create_fileref", "file_ref* create_fileref("),
                     ("add_dir_child", "NTSTATUS add_dir_child(")]:
    (out / f"{name}.inc").write_text(license_header + function(create, marker))
start = btrfs.index("#ifdef DEBUG_FCB_REFCOUNTS\nvoid _free_fcb(")
(out / "free_fcb.inc").write_text(license_header + btrfs[start:btrfs.index("\nvoid reap_fcb(", start)])
for name in ["reap_fcb", "free_fileref", "reap_fileref", "reap_fcbs", "reap_filerefs",
             "mark_fcb_dirty", "mark_fileref_dirty"]:
    (out / f"{name}.inc").write_text(license_header + function(btrfs, f"void {name}("))
(out / "add_fcb_to_subvol.inc").write_text(license_header + function(read("src/fileinfo.c"), "void add_fcb_to_subvol("))
(out / "btrfs.h").write_text(read("src/btrfs.h"))
crc = read("src/crc32c.c")
(out / "crc32c.inc").write_text(license_header + crc[crc.index("const uint32_t crctable[]"):])
binary = out / "create_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
if args.debug_refs:
    flags += ["-DDEBUG_FCB_REFCOUNTS"]
subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-fshort-wchar", "-O0", "-g",
                "-Wall", "-Wextra", "-Werror", f"-ftrivial-auto-var-init={args.init}", *flags,
                "-I", str(out), str(Path(__file__).with_name("create_test.c")), "-o", str(binary)], check=True)
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
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init}, debug refs: {args.debug_refs})", flush=True)
raise SystemExit(bool(failed))
