#!/usr/bin/env python3
"""Build production changed-extent replay and deterministic rationalization traces."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision", default=None)
parser.add_argument("--trace-revision", default="9da8aa72")
parser.add_argument("--compare", default="9da8aa72")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--bench", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(exist_ok=True, parents=True)
here = Path(__file__).resolve().parent


def read(path, revision=None, optional=False):
    if revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{revision}:{path}"],
                                capture_output=True, text=True, timeout=30)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if not result.returncode else ""
    file = repo / path
    return file.read_text() if file.exists() or not optional else ""


def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    end, depth = brace + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


source = read("src/changed-extents.c", args.revision, optional=True)
if source:
    private = function(source, "static uint32_t find_extent_data_refcount(")
    source = source.replace(private, "")
    code = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    code = re.sub(r'(?m)^(changed_extent\* get_changed_extent_item|NTSTATUS (?:update|add)_changed_extent_ref)',
                  r'static \1', code)
else:
    source = read("src/extent-tree.c", args.revision)
    code = "".join("static " + function(source, marker) for marker in (
        "changed_extent* get_changed_extent_item(", "NTSTATUS update_changed_extent_ref(",
        "NTSTATUS add_changed_extent_ref("))
(out / "refs.inc").write_text(code)
has_index = "void initialize_changed_extents(" in code
types = read("src/chunk-types.h", args.revision, optional=True) or read("src/btrfs_drv.h", args.revision)
types_end = types.index("} changed_extent_ref;") + len("} changed_extent_ref;")
types_start = types.rfind("typedef struct", 0, types.index("} changed_extent;"))
(out / "extent-type.inc").write_text(types[types_start:types_end] + "\n")
counted = re.sub(r'\ble = le->Flink;', 'le = (loop_steps++, le->Flink);', code)
counted = re.sub(r'(static int compare_changed_extent\([^\{]+\{)', r'\1\n    loop_steps++;', counted)
(out / "counted.inc").write_text(counted)
(out / "btrfs.h").write_text(read("src/btrfs.h", args.revision))
flags = ["-O2"] if args.bench else ["-O0", "-g", f"-ftrivial-auto-var-init={args.init}"]
if args.sanitize:
    if args.bench:
        parser.error("sanitizers are for semantics, not timing")
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
gcc = ["gcc", "-std=c11", *flags, "-Wall", "-Wextra", "-Werror",
       f"-DHAVE_INDEX={int(has_index)}",
       "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(out), "-I", str(here)]
cleanup = read("src/mount-cleanup.c", args.revision, optional=True)
terminal_marker = "while (!IsListEmpty(&c->changed_extents)) {"
has_terminal_drain = terminal_marker in cleanup
if has_terminal_drain:
    terminal = function(cleanup, terminal_marker)
    following = cleanup[cleanup.index(terminal) + len(terminal):].lstrip()
    if following.startswith("initialize_changed_extents(c);"):
        terminal += following.splitlines()[0] + "\n"
    (out / "terminal.inc").write_text("static void terminal_drain(chunk* c) {\n" + terminal + "}\n")
else:
    (out / "terminal.inc").write_text("")
gcc.append(f"-DHAVE_TERMINAL_DRAIN={int(has_terminal_drain)}")
subprocess.run([*gcc, str(here / "replay.c"), "-o", str(out / "replay")], check=True)
if not args.bench:
    subprocess.run([*gcc, str(here / "index_test.c"), "-o", str(out / "index_test")], check=True)
    subprocess.run([str(out / "index_test")], check=True)
    if has_index:
        old = read("src/extent-tree.c", args.compare)
        baseline = "".join("static " + function(old, marker) for marker in (
            "changed_extent* get_changed_extent_item(", "NTSTATUS update_changed_extent_ref(",
            "NTSTATUS add_changed_extent_ref("))
        (out / "baseline.inc").write_text(baseline)

        def lifecycle(revision, prefix):
            flush = read("src/changed-extent-flush.c", revision, optional=True)
            if not flush:
                flush = read("src/flushthread.c", revision)
            marker = "NTSTATUS flush_changed_extent("
            if "static " + marker in flush:
                marker = "static " + marker
            (out / f"{prefix}flush.inc").write_text(function(flush, marker))
            rollback = read("src/rollback.c", revision, optional=True)
            if not rollback:
                rollback = read("src/treefuncs.c", revision)
            case = function(rollback, "case ROLLBACK_UPDATE_CHANGED_EXTENT_REF: {")
            wrapper = ("static NTSTATUS rollback_one(device_extension* Vcb, rollback_item* ri) {\n"
                       "    NTSTATUS Status = STATUS_SUCCESS;\n    switch (ri->type) {\n" + case +
                       "    }\n    ExFreePool(ri);\n    return Status;\nfail:\n    return Status;\n}\n")
            (out / f"{prefix}rollback.inc").write_text(wrapper)

        lifecycle(args.revision, "")
        lifecycle(args.compare, "baseline-")
        subprocess.run([*gcc, str(here / "compare_test.c"), "-o", str(out / "compare_test")], check=True)
        subprocess.run([str(out / "compare_test")], check=True)

# Freeze both the layout generator and rationalization implementation to one revision.
# This keeps before/after workloads identical even when later source changes merge.
trace_src = out / "trace-src"
trace_src.mkdir(exist_ok=True)
(trace_src / "btrfs.h").write_text(read("src/btrfs.h", args.trace_revision))
(trace_src / "trace.c").write_text((here / "trace.c").read_text())
(trace_src / "trace-host.h").write_text((here / "trace-host.h").read_text())
ranges = read("src/extent-ranges.c", args.trace_revision, optional=True)
if not ranges:
    ranges = read("src/flushthread.c", args.trace_revision)
type_end = ranges.index("} extent_range;") + len("} extent_range;")
type_start = ranges.rindex("typedef struct {", 0, type_end)
last = ranges.index("NTSTATUS rationalize_extents(", type_end)
end_body = last + len(function(ranges, "NTSTATUS rationalize_extents(")) - 1
range_code = ranges[type_start:end_body]
range_code = re.sub(r"\bstatic\s+(?=NTSTATUS rationalize_extents\()", "", range_code)
(trace_src / "ranges.inc").write_text(range_code)
subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-I", str(trace_src),
                str(trace_src / "trace.c"),
                "-o", str(trace_src / "dump")], check=True)
traces = {}
for count in (40000, 87000):
    trace = out / f"trace-{count}.txt"
    subprocess.run([str(trace_src / "dump"), str(count), str(trace)], check=True)
    text = trace.read_text()
    header, calls = text.split("\n", 1)
    trace.write_text(header.split()[0] + " 0.000\n" + calls)
    traces[str(count)] = hashlib.sha256(trace.read_bytes()).hexdigest()
    if not args.bench:
        subprocess.run([str(out / "replay"), str(trace), "0"], check=True)
metadata = {"source": args.revision or "working-tree", "trace_revision": args.trace_revision,
            "source_sha256": hashlib.sha256(code.encode()).hexdigest(),
            "fixture_sha256": hashlib.sha256((here / "trace.c").read_bytes() +
                                               (here / "trace-host.h").read_bytes()).hexdigest(),
            "type_sha256": hashlib.sha256(types[types_start:types_end].encode()).hexdigest(),
            "trace_sha256": traces, "optimized": args.bench}
(out / "provenance.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps(metadata, sort_keys=True))
