#!/usr/bin/env python3
"""Exercise production metadata write assembly and count copies/comparisons.

Kernel allocation, chunks and lists are bounded host boundaries. Physical I/O
is excluded; the normal driver and VM suite exercise that unchanged path.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--revision")
parser.add_argument("--baseline", action="store_true")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--phase", choices=["coalescing", "ordering", "all"], default="all")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
here = Path(__file__).resolve().parent
out.mkdir(parents=True, exist_ok=True)

def read(name):
    if args.revision:
        return subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{name}"],
                              check=True, capture_output=True, text=True).stdout
    return (repo / name).read_text()

source, header = read("src/tree-write.c"), read("src/btrfs_drv.h")
end = header.index("} tree_write;") + len("} tree_write;")
types = header[header.rfind("typedef struct {", 0, end):end]
start = source.index("NTSTATUS do_tree_writes(")
end = source.index("    num_bits = 0;", start)
prefix = source[start:end] + "    test_raid56 = raid56;\n    return STATUS_SUCCESS;\n}\n"
helpers = source[source.index('\n', source.index('#include "crc32c.h"')) + 1:start]
# Count only real production address comparisons; do not replace the algorithm.
helpers = helpers.replace("left->address <= right->address", "address_before_equal(left->address, right->address)")
helpers = helpers.replace("tw->address > next->address", "address_after(tw->address, next->address)")
has_sort = "static void sort_tree_writes(" in helpers
if has_sort:
    insert = "InsertTailList(writes, &tw->list_entry);"
    assert "            InsertTailList(&tree_writes, &tw->list_entry);" in source
    assert "    sort_tree_writes(&tree_writes);\n    Status = do_tree_writes" in source
    finish = "sort_tree_writes(writes);"
else:
    pos = source.index("            if (IsListEmpty(&tree_writes))", source.index("NTSTATUS write_trees("))
    stop = source.index("\n        }\n\n        le = le->Flink;", pos)
    insert = source[pos:stop].replace("&tree_writes", "writes").replace("tree_writes.Flink", "writes->Flink")
    insert = insert.replace("tw2->address > tw->address", "address_after(tw2->address, tw->address)")
    finish = "(void)writes;"
unit = '#include "host.h"\n' + types + '\n' + helpers + prefix
unit += "static void insert_actual(LIST_ENTRY* writes, tree_write* tw) { LIST_ENTRY* le2;\n" + insert + "\n}\n"
unit += "static void finish_order(LIST_ENTRY* writes) { " + finish + " }\n"
unit += '#include "assembly_test.c"\n'
(out / "assembly.c").write_text(unit)
flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
         "-Wno-unused-variable", "-Wno-unused-but-set-variable", f"-ftrivial-auto-var-init={args.init}",
         f"-DHAS_SORT={int(has_sort)}"]
if args.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
binary = out / "tree-write-test"
subprocess.run(["gcc", *flags, "-I", str(here), str(out / "assembly.c"), "-o", str(binary)], check=True)
run = subprocess.run([str(binary), args.phase, "baseline" if args.baseline else "candidate"], capture_output=True, text=True)
(out / "stderr.log").write_text(run.stderr)
print(run.stdout, end="")
if run.returncode not in (0, 1):
    raise RuntimeError(f"assembly controls failed ({run.returncode}): {run.stderr}")
report = json.loads(run.stdout)
report.update(source_sha256=hashlib.sha256(source.encode()).hexdigest(),
              revision=args.revision or "working-tree", sanitizer=args.sanitize, initialization=args.init,
              measurement="production copy bytes, pool requests and address comparisons; no timing claim")
(out / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
raise SystemExit(run.returncode)
