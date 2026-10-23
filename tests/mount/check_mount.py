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

btrfs = read("src/btrfs.c") + read("src/volume-lifecycle.c", optional=True)
mount_source = read("src/mount.c", optional=True) or btrfs
mount = block(mount_source, mount_source.index("NTSTATUS mount_vol("))
device_start = mount.index("    dev = ExAllocatePoolWithTag(")
device_end = mount.index("    dev->devobj = readobj;", device_start)
(out / "device_init.inc").write_text(
    "static NTSTATUS allocate_first_device(device** result) {\n    device* dev;\n    NTSTATUS Status;\n" +
    mount[device_start:device_end] + "    *result = dev;\n    return STATUS_SUCCESS;\nexit:\n    return Status;\n}\n")
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
(out / "reap_fileref.inc").write_text(block(btrfs, btrfs.index("void reap_fileref(")) + "\n")
(out / "mark_dirty.inc").write_text(block(btrfs, btrfs.index("void mark_fcb_dirty(")) + "\n")
treefuncs = read("src/treefuncs.c")
search = read("src/tree-search.c", optional=True)
ownership = "".join(block(search, search.index(f"void {name}(")) + "\n"
                    for name in ("initialize_tree_search", "invalidate_tree_search")) if "void invalidate_tree_search(" in search else ""
(out / "trees.inc").write_text(ownership + "".join(block(treefuncs, treefuncs.index(f"void {name}(")) + "\n"
                                       for name in ("free_tree", "free_trees_root")))
batch_source = read("src/batch.c", optional=True) or treefuncs
(out / "clear_batch.inc").write_text(block(batch_source, batch_source.index("void clear_batch_list(")) + "\n")
batch_start = mount.index("    Status = commit_batch_list(Vcb, &batchlist, Irp);")
batch_end = mount.index("    Vcb->volume_fcb =", batch_start)
(out / "batch_failure.inc").write_text(
    "static NTSTATUS fail_batch_commit(device_extension* Vcb, LIST_ENTRY* batchlist) {\n    NTSTATUS Status;\n    void* Irp = NULL;\n" +
    mount[batch_start:batch_end].replace("&batchlist", "batchlist") + "exit:\n    return Status;\n}\n")
metadata_source = read("src/mount-metadata.c", optional=True) or btrfs
chunk_load = block(metadata_source, metadata_source.index("NTSTATUS load_chunk_root("))
missing = chunk_load.index("if (!c->devices[i])")
(out / "missing_device.inc").write_text(
    "static NTSTATUS fail_chunk_device(chunk* c, unsigned i) {\n    " + block(chunk_load, missing) +
    "\n    return STATUS_SUCCESS;\n}\n")
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
