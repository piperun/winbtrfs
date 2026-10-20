#!/usr/bin/env python3
"""Run actual WinBtrfs mknod and lookup routines with host service mocks."""
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
        p = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                           text=True, capture_output=True)
        if p.returncode and not optional:
            raise RuntimeError(p.stderr)
        return p.stdout if p.returncode == 0 else ""
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

source = read("src/mknod.c", optional=True) or read("src/fsctl.c")
license_header = source[:source.index("#include")]
(out / "mknod.inc").write_text(license_header + function(source, "NTSTATUS mknod("))
(out / "check_inode_used.inc").write_text(license_header + function(source, "static NTSTATUS check_inode_used("))
create = read("src/create.c")
(out / "create_fcb.inc").write_text(license_header + function(create, "fcb* create_fcb("))
(out / "create_fileref.inc").write_text(license_header + function(create, "file_ref* create_fileref("))
btrfs = read("src/btrfs.c")
start = btrfs.index("#ifdef DEBUG_FCB_REFCOUNTS\nvoid _free_fcb(")
(out / "free_fcb.inc").write_text(license_header + btrfs[start:btrfs.index("\nvoid reap_fcb(", start)])
for name in ["reap_fcb", "free_fileref", "reap_fileref", "reap_fcbs", "reap_filerefs",
             "mark_fcb_dirty", "mark_fileref_dirty"]:
    (out / f"{name}.inc").write_text(license_header + function(btrfs, f"void {name}("))
lookup = read("src/fileref-open.c", optional=True)
for marker, filename in [("NTSTATUS find_file_in_dir(", "find_file_in_dir.inc"),
                         ("NTSTATUS add_dir_child(", "add_dir_child.inc")]:
    (out / filename).write_text(license_header + function(lookup if marker in lookup else create, marker))
(out / "add_fcb_to_subvol.inc").write_text(license_header + function(read("src/fileinfo.c"), "void add_fcb_to_subvol("))
(out / "btrfs.h").write_text(read("src/btrfs.h"))
protocol = read("src/btrfsioctl.h")
end = protocol.index("} btrfs_mknod;") + len("} btrfs_mknod;")
start = protocol.rfind("typedef struct {", 0, end)
(out / "protocol.inc").write_text(license_header + protocol[start:end] + "\n")
crc = read("src/crc32c.c")
(out / "crc32c.inc").write_text(license_header + crc[crc.index("const uint32_t crctable[]"):])
binary = out / "mknod_test"
sanitizers = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *sanitizers, "-I", str(out),
                str(Path(__file__).with_name("mknod_test.c")), "-o", str(binary)], check=True)
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=5)
        if result.returncode:
            print(f"FAIL {case}: exit {result.returncode}", flush=True)
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: five-second timeout", flush=True)
        failed += 1
print(f"{len(cases) - failed}/{len(cases)} scenarios passed (automatic locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
