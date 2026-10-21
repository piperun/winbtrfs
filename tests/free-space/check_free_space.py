#!/usr/bin/env python3
"""Check production space-list merging against an interval oracle and visit bounds."""
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

def read(path):
    if args.revision:
        return subprocess.check_output(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                       text=True, timeout=10)
    return (repo / path).read_text()

def function(source, marker):
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

source = read("src/free-space.c")
helper = "static NTSTATUS space_list_add_from(" in source
# Marker, the counter for its list steps, and whether it must walk a list. The
# helpers are absent before the one-pass merge.
functions = [("static void order_space_entry(", "size_visits", True),
             ("static void place_space_entry(", "size_visits", False),
             ("static NTSTATUS space_list_add_from(", "address_visits", True),
             ("NTSTATUS space_list_add2(", "address_visits", not helper),
             ("static void order_space_entries(", "size_visits", True),
             ("NTSTATUS space_list_merge(", "address_visits", True)]
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
    if marker not in source:
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
for name, text in (("counted", counted), ("bench", plain)):
    (out / name).mkdir(exist_ok=True)
    (out / name / "space.inc").write_text(text)

if args.bench:
    binary = out / "free_space_bench"
    subprocess.run(["gcc", "-std=c11", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                    "-I", str(out / "bench"), "-I", str(out), str(test), "-o", str(binary)],
                   check=True, timeout=60)
    raise SystemExit(subprocess.run([str(binary), "--bench"], timeout=900).returncode)

binary = out / "free_space_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-O0", "-g", "-Wall", "-Wextra", "-Werror",
                f"-ftrivial-auto-var-init={args.init}", *flags, f"-DMERGE_HOOKS={int(helper)}",
                "-I", str(out / "counted"), "-I", str(out), str(test), "-o", str(binary)],
               check=True, timeout=60)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = subprocess.check_output([str(binary), "--list"], text=True, timeout=5).splitlines()
failed = 0
for case in cases:
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
