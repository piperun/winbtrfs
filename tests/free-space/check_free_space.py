#!/usr/bin/env python3
"""Check production space-list merging and the cache writers and loaders that use it."""
import argparse
from pathlib import Path
import re
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision", help="Git revision; omit for working-tree sources")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--bench", action="store_true",
                    help="Only time merges, with an optimized build and no step counters")
args = parser.parse_args()
if args.bench and args.sanitize:
    parser.error("--bench times an optimized build without sanitizers")
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path, optional=False):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                text=True, capture_output=True, timeout=10)
        if result.returncode and not optional: raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""


def function(source, marker):
    if marker not in source and marker.startswith("static "): marker = marker[7:]
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

header = read("src/btrfs_drv.h")
end = header.index("} space;") + len("} space;")
(out / "types.inc").write_text(header[header.rindex("typedef struct {", 0, end):end] + "\n")

source = read("src/free-space.c") + read("src/space-list.c", optional=True) + read("src/cache-allocation.c", optional=True) + read("src/cache-write.c", optional=True)
helper = "static NTSTATUS space_list_add_from(" in source
# Marker, the counter for its list steps, and whether it must walk a list. The
# helpers are absent before the one-pass merge. The loaders add entries with
# add_space_entry; allocations take space with space_list_subtract2.
functions = [("static void order_space_entry(", "size_visits", True),
             ("static void place_space_entry(", "size_visits", False),
             ("static NTSTATUS space_list_add_from(", "address_visits", True),
             ("NTSTATUS space_list_add2(", "address_visits", not helper),
             ("static void order_space_entries(", "size_visits", True),
             ("NTSTATUS space_list_merge(", "address_visits", True),
             ("NTSTATUS add_space_entry(", "address_visits", True),
             ("NTSTATUS space_list_subtract2(", "address_visits", False)]
# Calls into the test where each insert starts, where an entry takes a range,
# where entries are resized or freed, where the merge restarts from the head and
# where an allocation fails, with the number of places each pattern must match.
hooks = {"static NTSTATUS space_list_add_from(": [
             (r"(?m)^( +)le = start;$", r"\1le = start;\n\1note_start(list, start);", 1),
             (r"\*ps = (\w+);", r"*ps = note_holder(\1);", 8),
             (r"RemoveEntryList\(&s2->list_entry_size\);", r"note_resize(touched, s2); \g<0>", 4),
             (r"RemoveEntryList\(&s3->list_entry\);", r"note_free(touched, s3); \g<0>", 4)],
         "NTSTATUS space_list_merge(": [
             (r"(?m)^( +)start = spacelist->Flink;$",
              r"\1{ note_restart(start, spacelist->Flink); start = spacelist->Flink; }", 1),
             (r"(?m)^( +)break;$", r"\1{ note_failure(&touched); break; }", 1)]}
plain, counted = "", ""
for marker, counter, walks in functions:
    if marker not in source and not (marker.startswith("static ") and marker[7:] in source):
        continue
    text = function(source, marker)
    # Every cursor step through a list has the form `le = le->Flink`.
    instrumented, found = re.subn(r"\b(\w+) = \1->Flink\b", rf"\1 = ({counter}++, \1->Flink)", text)
    if walks and not found:
        raise SystemExit(f"no list steps to count in {marker}")
    for pattern, replacement, places in (hooks.get(marker, []) if helper else []):
        instrumented, found = re.subn(pattern, replacement, instrumented)
        if found != places:
            raise SystemExit(f"{pattern} matched {found} places in {marker}, expected {places}")
    plain += text
    counted += instrumented

test = Path(__file__).with_name("free_space_test.c")
for name, text in (("counted", counted), ("plain", plain)):
    (out / name).mkdir(exist_ok=True)
    (out / name / "space.inc").write_text(text)

if args.bench:
    binary = out / "free_space_bench"
    subprocess.run(["gcc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                    "-I", str(out / "plain"), "-I", str(out), str(test), "-o", str(binary)],
                   check=True, timeout=60)
    raise SystemExit(subprocess.run([str(binary), "--bench"], timeout=900).returncode)

# The cache writers copy the chunk's lists and merge them with the plain functions.
(out / "cache.inc").write_text("".join(function(source, marker) for marker in (
    "static NTSTATUS copy_space_list(", "static NTSTATUS update_chunk_cache(",
    "static NTSTATUS update_chunk_cache_tree(")))
pending = read("src/trim-reservations.c", optional=True)
if pending:
    (out / "pending.inc").write_text("typedef struct { LIST_ENTRY list_entry, chunk_entry, ranges; chunk* owner; } trim_chunk;\n" + function(pending, "NTSTATUS trim_merge_pending("))
else:
    (out / "pending.inc").write_text("typedef struct { LIST_ENTRY list_entry, chunk_entry, ranges; chunk* owner; } trim_chunk;\nstatic NTSTATUS trim_merge_pending(chunk* c, LIST_ENTRY* l, LIST_ENTRY* s) { (void)c; (void)l; (void)s; return STATUS_SUCCESS; }\n")
# The loaders add stored entries with the plain add_space_entry, the v1 loader the
# runs of a bitmap through load_free_space_bitmap. That returned void before it
# reported failures, so its marker takes any return type. load_cache_chunk runs the
# loaders, and the test's protect_superblocks takes the superblock stripes off the
# loaded entries with space_list_subtract.
bitmap = re.search(r"static \w+ load_free_space_bitmap\(", source)[0]
bitmap_code = function(source, bitmap).replace(
    "{\n", "{\n    observe_cache_bitmap(data, Vcb->superblock.sectorsize);\n", 1)
cache_code = function(source, "NTSTATUS load_stored_free_space_cache(")
cache_code, observed = re.subn(
    r"(?m)^( +)fse = \(struct btrfs_free_space_entry\*\)&data\[off\];$",
    r"\1observe_cache_entry(data, off, size);\n\g<0>", cache_code)
if observed != 2:
    raise SystemExit(f"expected two cache-entry reads, found {observed}")
(out / "load.inc").write_text(bitmap_code + cache_code + "".join(function(source, marker) for marker in (
    "static NTSTATUS load_stored_free_space_tree(",
    "static NTSTATUS load_free_space_cache(", "NTSTATUS load_cache_chunk(",
    "NTSTATUS space_list_subtract(")))
(out / "btrfs.h").write_text(read("src/btrfs.h"))

# Exercise the production dirty-list ownership transitions without pulling the
# tree-writing dependencies of flush_fcb into the host model.
flush = read("src/flushthread.c") + read("src/transaction-chunks.c", optional=True) + read("src/batch-insert.c", optional=True)
flush_body = function(flush, "NTSTATUS flush_fcb(")
cleanup = flush_body[flush_body.rindex("\nend:"):]
(out / "flush_ref.inc").write_text(
    function(read("src/btrfs.c"), "void mark_fcb_dirty(") +
    "NTSTATUS flush_fcb(fcb* fcb) {\n"
    "    NTSTATUS Status = flush_status;\n"
    "    goto end;\n" + cleanup)

flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
binaries = []
for name, program, functions_dir, defines in (
        ("free_space_test", test, "counted", [f"-DMERGE_HOOKS={int(helper)}"]),
        ("cache_test", test.with_name("cache_test.c"), "plain", []),
        ("load_test", test.with_name("load_test.c"), "plain", []),
        ("flush_ref_test", test.with_name("flush_ref_test.c"), "plain", [])):
    binary = out / name
    subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                    f"-ftrivial-auto-var-init={args.init}", *flags, *defines,
                    "-I", str(out / functions_dir), "-I", str(out), str(program), "-o", str(binary)],
                   check=True, timeout=60)
    binaries.append(binary)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = [(binary, case) for binary in binaries for case in
         subprocess.check_output([str(binary), "--list"], text=True, timeout=5).splitlines()]
failed = 0
for binary, case in cases:
    # A quadratic merge takes seconds to finish counting, so it gets a longer limit.
    limit = 120 if case.startswith("complexity") else 30
    try:
        result = subprocess.run([str(binary), case], timeout=limit)
        if result.returncode:
            print(f"FAIL {case}: exit {result.returncode}", flush=True)
            failed += 1
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: {limit}-second timeout", flush=True)
        failed += 1
print(f"{len(cases)-failed}/{len(cases)} scenarios passed (locals: {args.init})", flush=True)
raise SystemExit(bool(failed))
