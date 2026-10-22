#!/usr/bin/env python3
"""Exercise real fileref and FCB payload producers with the production queue."""
import argparse
import os
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def block(source, marker):
    start = source.index(marker)
    end, depth = source.index("{", start) + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

header = (repo / "src/btrfs_drv.h").read_text()
start = header.index("enum batch_operation {")
end = header.index("} batch_root;", start) + len("} batch_root;")
(out / "batch_types.inc").write_text(header[start:end] + "\n")
(out / "btrfs.h").write_text((repo / "src/btrfs.h").read_text())
source = (repo / "src/flushthread.c").read_text()
start = source.rindex("static NTSTATUS insert_tree_item_batch(")
limit = next(line for line in source.splitlines() if line.startswith("#define BATCH_ITEM_LIMIT "))
(out / "queue.inc").write_text(limit + "\n" + block(source, "static NTSTATUS split_batch_item_list(") +
                               block(source[start:], "static NTSTATUS insert_tree_item_batch("))
(out / "fileref.inc").write_text(block(source, "static NTSTATUS flush_fileref("))
(out / "clear.inc").write_text(block((repo / "src/batch.c").read_text(), "void clear_batch_list("))
(out / "sparse.inc").write_text(block(source, "static NTSTATUS insert_sparse_extent("))
fcb = block(source, "NTSTATUS flush_fcb(")
begin = fcb.index("last_end = 0;", fcb.index("// add new EXTENT_DATAs"))
end = fcb.index("\n        if (!(fcb->Vcb->superblock.incompat_flags", begin)
# Include the full extent loop, not just its allocation/error branch.
(out / "extents.inc").write_text(fcb[begin:end] + "\n")
(out / "inode.inc").write_text(block(fcb, "if (!cache && fcb->inode_item_changed)"))
(out / "fcb_end.inc").write_text(fcb[fcb.rindex("\nend:"):])

binary = out / "batch_producers_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, "-I", str(out),
                str(Path(__file__).with_name("batch_producers_test.c")), "-o", str(binary)], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
for case in cases:
    result = subprocess.run([str(binary), case], env=env, timeout=5)
    if result.returncode:
        print(f"FAIL {case}: exit {result.returncode}", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} producer scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
