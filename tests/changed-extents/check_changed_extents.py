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
(out / "btrfs.h").write_text(read("src/btrfs.h", args.revision))
flags = ["-O2"] if args.bench else ["-O0", "-g", f"-ftrivial-auto-var-init={args.init}"]
if args.sanitize:
    if args.bench:
        parser.error("sanitizers are for semantics, not timing")
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
gcc = ["gcc", "-std=c11", *flags, "-Wall", "-Wextra", "-Werror",
       "-Wno-unused-function", "-Wno-unused-parameter", "-I", str(out), "-I", str(here)]
subprocess.run([*gcc, str(here / "replay.c"), "-o", str(out / "replay")], check=True)

# Freeze both the layout generator and rationalization implementation to one revision.
# This keeps before/after workloads identical even when later source changes merge.
trace_src = out / "trace-src"
trace_src.mkdir(exist_ok=True)
for name in ("compare_test.c", "host.h", "impl.c"):
    (trace_src / name).write_text(read(f"tests/extents/{name}", args.trace_revision))
(trace_src / "btrfs.h").write_text(read("src/btrfs.h", args.trace_revision))
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
                "-Drationalize_extents=candidate_rationalize_extents", "-I", str(trace_src),
                "-c", str(trace_src / "impl.c"), "-o", str(trace_src / "candidate.o")], check=True)
subprocess.run(["gcc", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                "-Wno-unused-function", "-DHAVE_BASELINE=0", "-I", str(trace_src),
                str(here / "trace.c"), str(trace_src / "candidate.o"),
                "-o", str(trace_src / "dump")], check=True)
traces = {}
for count in (40000, 87000):
    trace = out / f"trace-{count}.txt"
    if not trace.exists():
        subprocess.run([str(trace_src / "dump"), str(count), str(trace)], check=True)
    text = trace.read_text()
    header, calls = text.split("\n", 1)
    trace.write_text(header.split()[0] + " 0.000\n" + calls)
    traces[str(count)] = hashlib.sha256(trace.read_bytes()).hexdigest()
    if not args.bench:
        subprocess.run([str(out / "replay"), str(trace), "0"], check=True)
metadata = {"source": args.revision or "working-tree", "trace_revision": args.trace_revision,
            "trace_sha256": traces, "optimized": args.bench}
(out / "provenance.json").write_text(json.dumps(metadata, indent=2) + "\n")
print(json.dumps(metadata, sort_keys=True))
