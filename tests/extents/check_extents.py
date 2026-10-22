#!/usr/bin/env python3
"""Exercise production extent rationalization: reference-update failures, random
extent lists compared with another revision, loop-step bounds and timings."""
import argparse
from pathlib import Path
import re
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision", help="Git revision; omit for working-tree sources")
parser.add_argument("--compare", metavar="REVISION",
                    help="Also run every random trial with rationalize_extents from this git "
                         "revision and require the same calls, extents and status")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
parser.add_argument("--bench", action="store_true",
                    help="Only time the function, optimized and without loop counters")
args = parser.parse_args()
if args.bench and args.sanitize:
    parser.error("--bench times an optimized build without sanitizers")
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent

def read(path, revision, optional=False):
    if revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{revision}:{path}"],
                                text=True, capture_output=True, timeout=30)
        if result.returncode and not optional:
            raise RuntimeError(result.stderr)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() or not optional else ""

def ranges_code(revision):
    """The extent_range type, any helpers defined after it, and rationalize_extents."""
    source = read("src/extent-ranges.c", revision, optional=True) or read("src/flushthread.c", revision)
    end = source.index("} extent_range;") + len("} extent_range;")
    start = source.rindex("typedef struct {", 0, end)
    marker = source.index("NTSTATUS rationalize_extents(", end)
    brace = source.index("{", marker)
    end_function, depth = brace + 1, 1
    while depth:
        depth += (source[end_function] == "{") - (source[end_function] == "}")
        end_function += 1
    # flushthread.c makes it static; the comparison build calls it from another file.
    code = re.sub(r"\bstatic\s+(?=NTSTATUS rationalize_extents\()", "", source[start:end_function])
    return code + "\n"

def split_top(text):
    parts, depth, last = [], 0, 0
    for i, ch in enumerate(text):
        depth += (ch == "(") - (ch == ")")
        if ch == ";" and depth == 0:
            parts.append(text[last:i])
            last = i + 1
    return parts + [text[last:]]

def count_loops(code):
    """Add one to loop_steps each time a while or for condition is evaluated."""
    pieces, pos, found = [], 0, 0
    for m in re.finditer(r"\b(while|for)\s*\(", code):
        if m.start() < pos:
            continue
        open_paren = m.end()
        close, depth = open_paren, 1
        while depth:
            depth += (code[close] == "(") - (code[close] == ")")
            close += 1
        inner = code[open_paren:close - 1]
        if m.group(1) == "while":
            inner = f"(loop_steps++, {inner})"
        else:
            parts = split_top(inner)
            if len(parts) != 3:
                raise SystemExit(f"cannot count the loop at: {code[m.start():close]}")
            init, condition, step = (p.strip() for p in parts)
            inner = f"{init}; (loop_steps++, {condition or '1'}); {step}"
        pieces.append(code[pos:open_paren] + inner)
        pos = close - 1
        found += 1
    if not found:
        raise SystemExit("no loops to count in rationalize_extents")
    return "".join(pieces) + code[pos:]

(out / "btrfs.h").write_text(read("src/btrfs.h", args.revision))
functions = {"candidate": ranges_code(args.revision)}
if args.compare:
    functions["baseline"] = ranges_code(args.compare)
if args.bench:
    flags, optimize = [], "-O2"
else:
    flags = [f"-ftrivial-auto-var-init={args.init}"]
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
    optimize = "-O0"
gcc = ["gcc", "-std=c11", optimize, "-g", "-Wall", "-Wextra", "-Werror", *flags]

def build_compare(counted):
    objects = []
    for name, code in functions.items():
        directory = out / ("counted" if counted else "plain") / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "ranges.inc").write_text(count_loops(code) if counted else code)
        objects.append(directory / "ranges.o")
        subprocess.run([*gcc, f"-Drationalize_extents={name}_rationalize_extents",
                        "-I", str(directory), "-I", str(out), "-c", str(here / "impl.c"),
                        "-o", str(objects[-1])], check=True, timeout=60)
    binary = out / ("compare_test" if counted else "compare_bench")
    subprocess.run([*gcc, f"-DHAVE_BASELINE={int(args.compare is not None)}", "-I", str(out),
                    str(here / "compare_test.c"), *map(str, objects), "-o", str(binary)],
                   check=True, timeout=60)
    return binary

if args.bench:
    raise SystemExit(subprocess.run([str(build_compare(counted=False)), "--bench"], timeout=1800).returncode)

single = out / "single"
single.mkdir(exist_ok=True)
(single / "ranges.inc").write_text(functions["candidate"])
extents_test = out / "extents_test"
subprocess.run([*gcc, "-I", str(single), "-I", str(out), str(here / "extents_test.c"),
                "-o", str(extents_test)], check=True, timeout=60)
compare_test = build_compare(counted=True)

resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
failed = total = 0
for binary in (extents_test, compare_test):
    cases = subprocess.check_output([str(binary), "--list"], text=True, timeout=5).splitlines()
    for case in cases:
        # Random trials and the large step-count cases take seconds, more with sanitizers.
        limit = 5 if binary == extents_test else 300
        total += 1
        try:
            result = subprocess.run([str(binary), case], timeout=limit)
            if result.returncode:
                print(f"FAIL {case}: exit {result.returncode}", flush=True)
                failed += 1
        except subprocess.TimeoutExpired:
            print(f"FAIL {case}: {limit}-second timeout", flush=True)
            failed += 1
compared = f", compared with {args.compare}" if args.compare else ""
print(f"{total-failed}/{total} scenarios passed (locals: {args.init}{compared})", flush=True)
raise SystemExit(bool(failed))
