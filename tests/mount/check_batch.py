#!/usr/bin/env python3
"""Execute actual batch commit and collision code with tracked native ownership."""
import argparse
from pathlib import Path
import os
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
    if marker not in source and marker.startswith("static "): marker = marker[7:]
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
source = (repo / "src/batch.c").read_text()
search = repo / "src/tree-search.c"
ownership = search.read_text() if search.exists() else ""
(out / "tree_search_lifecycle.inc").write_text("".join(block(ownership, f"void {name}(")
    for name in ("initialize_tree_search", "invalidate_tree_search")) if "void invalidate_tree_search(" in ownership else "")
collision = (repo / "src/batch-collision.c").read_text()
(out / "batch_commit.inc").write_text("".join(block(source, marker) for marker in (
    "static NTSTATUS find_tree_end(", "void clear_batch_list(", "static NTSTATUS commit_batch_list_root(", "NTSTATUS commit_batch_list(")))
(out / "batch_collision.inc").write_text("".join(block(collision, "NTSTATUS " + name + "(") for name in ("add_delete_inode_extref", "handle_batch_collision")))
producer = "\n".join((repo / path).read_text() for path in ["src/flushthread.c", "src/transaction-chunks.c", "src/batch-insert.c"] if (repo / path).exists())
producer_start = producer.rindex("NTSTATUS insert_tree_item_batch(")
limit = next(line for line in producer.splitlines() if line.startswith("#define BATCH_ITEM_LIMIT "))
(out / "batch_producer.inc").write_text(limit + "\n" + block(producer, "static NTSTATUS split_batch_item_list(") +
    block(producer[producer_start:], "static NTSTATUS insert_tree_item_batch("))
binary = out / "batch_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror", f"-ftrivial-auto-var-init={args.init}", *flags,
    "-I", str(out), str(Path(__file__).with_name("batch_test.c")), "-o", str(binary)], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
env = dict(os.environ, ASAN_OPTIONS="detect_leaks=1:abort_on_error=1", UBSAN_OPTIONS="halt_on_error=1")
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=5, env=env)
        if result.returncode:
            print(f"FAIL {case}: exit {result.returncode}", flush=True)
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: five-second timeout", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} batch scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
