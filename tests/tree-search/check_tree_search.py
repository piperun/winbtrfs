#!/usr/bin/env python3
"""Execute the real tree-search body and count comparisons on internal queries."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--revision")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--baseline", action="store_true")
parser.add_argument("--fixtures", type=Path)
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
here = Path(__file__).resolve().parent
out.mkdir(parents=True, exist_ok=True)

def read(name, optional=False):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{name}"], text=True, capture_output=True)
        if not result.returncode:
            return result.stdout
        if not optional:
            raise RuntimeError(result.stderr)
        return ""
    path = repo / name
    return path.read_text() if path.exists() or not optional else ""

source = read("src/tree-search.c", optional=True)
if not source:
    source = read("src/treefuncs.c")
    source = source[source.index("static __inline tree_data* first_item("):source.index("void free_trees_root(")]
    source = source[:source.rfind("__attribute__")]
source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
(out / "search.inc").write_text(source)
types = read("src/tree-types.h", optional=True)
if not types:
    header = read("src/btrfs_drv.h")
    start = header.index("typedef struct {\n    uint64_t address;\n    uint64_t generation;\n    struct _tree* tree;")
    types = header[start:header.index("} tree;", start) + len("} tree;")]
(out / "tree-types.h").write_text(types)
has_index = "void cache_tree_search(" in source
if has_index:
    (out / "tree-search.h").write_text(read("src/tree-search.h"))
flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-function",
         f"-ftrivial-auto-var-init={args.init}", f"-DHAVE_INDEX={int(has_index)}"]
if args.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
binary = out / "tree-search-test"
subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "search_test.c"), "-o", str(binary)], check=True)
subprocess.run([str(binary), *( ["baseline"] if args.baseline else [])], check=True)
binary = out / "tree-semantics-test"
subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "semantics_test.c"), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
binary = out / "tree-search-cost"
subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "cost_test.c"), "-o", str(binary)], check=True)
cost = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
report = dict(indexed=has_index, measurement="actual comparator calls and pool allocation bytes; no timing claim",
              source_sha256=hashlib.sha256(source.encode()).hexdigest(),
              cases=[json.loads(line) for line in cost.stdout.splitlines()])
(out / "cost.json").write_text(json.dumps(report, indent=2) + "\n")
print(cost.stdout, end="")
if args.fixtures:
    fixtures = json.loads(args.fixtures.read_text())
    arrays, calls = [], []
    for fixture in fixtures:
        if not fixture["eligible"]:
            continue
        record = fixture["record"]
        assert not record["adjacent_lookup"] and record["level"] == 0
        slot = fixture["slot"]
        keys = fixture["keys"]
        arrays.append(f"static const uint64_t keys_{slot}[][4] = {{\n" + ",\n".join(
            "{" + ",".join(f"{value}ULL" for value in row) + "}" for row in keys) + "\n};\n")
        query = "{" + ",".join(f"{value}ULL" for value in record["query"]) + "}"
        calls.append(f"replay({slot}, keys_{slot}, {len(keys)}, {record['live_items']}, (struct btrfs_key){query}, "
                     f"{int(record['include_ignored'])}, (NTSTATUS){record['status']}, {fixture['result_index']}ULL, {record['comparisons']}ULL);")
    (out / "captured-fixtures.inc").write_text("\n".join(arrays) + "\nint main(void) {\n" + "\n".join(calls) + "\nreturn 0;\n}\n")
    binary = out / "tree-fixture-test"
    subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "fixture_test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
if has_index and not args.baseline:
    balance = read("src/tree-balance.c")
    def region(text, anchor, ending):
        a = text.index(anchor)
        start = text.rfind("\n\n", 0, a) + 2
        end = text.index(ending, a)
        return text[start:end]
    split = region(balance, "    oldlastitem = CONTAINING_RECORD(newfirstitem", "    nt->size =")
    merge = region(balance, "        t->itemlist.Blink->Flink = next_tree->itemlist.Flink;", "        next_tree->header.nritems =")
    a = balance.index("                RemoveEntryList(&td->list_entry);")
    start = balance.rfind("{", 0, a) + 1
    end_marker = "                InsertTailList(&t->itemlist, &td->list_entry);"
    move = balance[start:balance.index(end_marker, a) + len(end_marker)]
    remove_sibling = region(balance, "        RemoveEntryList(&nextparitem->list_entry);", "        ExFreePool(next_tree->paritem);")
    remove_child = region(balance, "                        RemoveEntryList(&t->paritem->list_entry);", "                        ExFreePool(t->paritem);")
    convert = region(balance, "                        t->header.level = 0;", "                    }\n                } else if")
    def loop(text):
        a = text.index("paritem = tp.tree->paritem;")
        end = text.index("while (paritem)", a)
        end = text.index("{", end) + 1
        depth = 1
        while depth:
            depth += (text[end] == "{") - (text[end] == "}")
            end += 1
        return text[a:end]
    single = loop(read("src/treefuncs.c"))
    batch = loop(read("src/batch.c"))
    sibling_insert = region(balance, "        InsertHeadList(&t->paritem->list_entry, &td->list_entry);", "        td->ignore = false;")
    write = read("src/tree-write.c")
    a = write.index("if (t->parent) {", write.index("NTSTATUS write_trees("))
    end, depth = write.index("{", a) + 1, 1
    while depth:
        depth += (write[end] == "{") - (write[end] == "}")
        end += 1
    serialize = write[a:end]
    created = read("src/root-create.c")
    root_prefix = region(created, "        t->nonpaged = NULL;", "        RtlZeroMemory(&t->header,")
    a = balance.index("    if (!nt) {")
    end, depth = balance.index("{", a) + 1, 1
    while depth:
        depth += (balance[end] == "{") - (balance[end] == "}")
        end += 1
    sibling_prefix = balance[end:balance.index("    RtlCopyMemory(&nt->header,", end)]
    parent_prefix = region(balance, "    pt->nonpaged = ExAllocatePoolWithTag(", "    RtlCopyMemory(&pt->header,")
    wrappers = (
        "static void split_items(tree* t, tree* nt, tree_data* newfirstitem) { tree_data* oldlastitem;\n" + split + "}\n" +
        "static void merge_items(tree* t, tree* next_tree) {\n" + merge + "}\n" +
        "static void move_item(tree* t, tree* next_tree, tree_data* td) {(void)next_tree;\n" + move + "}\n" +
        "static void remove_sibling(tree* next_tree, tree_data* nextparitem) {(void)next_tree;\n" + remove_sibling + "}\n" +
        "static void remove_child(tree* t) {\n" + remove_child + "}\n" +
        "static void convert_empty_root(tree* t) {\n" + convert + "}\n" +
        "static void insert_rekey(traverse_ptr tp, struct btrfs_key searchkey) {tree_data* paritem;\n" + single + "}\n" +
        "static void batch_rekey(traverse_ptr tp, struct btrfs_key key) {tree_data* paritem; struct {struct btrfs_key key;} item={key}; __typeof__(item)* bi=&item;\n" + batch + "}\n" +
        "static void insert_separator(tree* t, tree_data* td) {\n" + sibling_insert + "}\n" +
        "static void serialize_separator(tree* t, struct btrfs_key firstitem) {device_extension volume={0}; device_extension* Vcb=&volume; Vcb->superblock.generation=77;\n" + serialize + "}\n" +
        "static void initialize_root_prefix(tree* t) {root value={0}; root* r=&value;\n" + root_prefix + "}\n" +
        "static NTSTATUS initialize_sibling_prefix(tree* t, tree* nt) {\n" + sibling_prefix + "return STATUS_SUCCESS;\n}\n" +
        "static NTSTATUS initialize_parent_prefix(tree* pt) {\n" + parent_prefix + "return STATUS_SUCCESS;\n}\n")
    (out / "mutations.inc").write_text(wrappers)
    binary = out / "tree-mutation-test"
    subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "mutation_test.c"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
def function(text, marker):
    start = text.index(marker)
    end = text.index("{", start) + 1
    depth = 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end] + "\n"

trees = read("src/treefuncs.c")
(out / "lifecycle.inc").write_text(function(read("src/tree-load.c", optional=True) or trees, "NTSTATUS load_tree(") + function(trees, "void free_tree("))
binary = out / "tree-lifecycle-test"
subprocess.run(["gcc", *flags, "-I", str(out), "-I", str(here), str(here / "lifecycle_test.c"), "-o", str(binary)], check=True)
subprocess.run([str(binary)], check=True)
