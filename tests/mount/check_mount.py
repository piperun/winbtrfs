#!/usr/bin/env python3
"""Execute failed-mount cleanup and production FCB destruction with tracked lifetimes."""
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
                                text=True, capture_output=True)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

def block(source, start):
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]

btrfs = read("src/btrfs.c")
mount_source = read("src/mount.c", optional=True) or btrfs
mount = block(mount_source, mount_source.index("NTSTATUS mount_vol("))
exit_start = mount.index("\nexit:\n") + len("\nexit:\n")
failure = mount.index("    if (!NT_SUCCESS(Status)) {", exit_start)
wrapper = "static NTSTATUS fail_mount(device_extension* Vcb, fcb* root_fcb, bool init_lookaside, NTSTATUS Status, PDEVICE_OBJECT NewDeviceObject) {\n"
wrapper += mount[exit_start:failure] + block(mount, failure) + "\n    return Status;\n}\n"
(out / "failure.inc").write_text(wrapper)
cleanup = read("src/mount-cleanup.c", optional=True)
(out / "cleanup.inc").write_text(block(cleanup, cleanup.index("void cleanup_failed_mount(")) + "\n" if cleanup else "")
start = btrfs.index("#ifdef DEBUG_FCB_REFCOUNTS\nvoid _free_fcb(")
(out / "free.inc").write_text(btrfs[start:btrfs.index("\nvoid reap_fcb(", start)])
(out / "reap.inc").write_text(block(btrfs, btrfs.index("void reap_fcb(")) + "\n")
(out / "reap_fcbs.inc").write_text(block(btrfs, btrfs.index("void reap_fcbs(")) + "\n")
(out / "free_fileref.inc").write_text(block(btrfs, btrfs.index("void free_fileref(")) + "\n")
treefuncs = read("src/treefuncs.c")
(out / "trees.inc").write_text("".join(block(treefuncs, treefuncs.index(f"void {name}(")) + "\n"
                                       for name in ("free_tree", "free_trees_root")))
binary = out / "mount_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("mount_test.c")), "-o", str(binary)], check=True)
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
