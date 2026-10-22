#!/usr/bin/env python3
"""Run the production fast write, write-behind and flush lock paths on pthreads."""
import argparse
from pathlib import Path
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--revision")
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo = args.repo.resolve()
out = args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)

def read(path):
    if args.revision:
        result = subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                                text=True, capture_output=True)
        return result.stdout if result.returncode == 0 else ""
    p = repo / path
    return p.read_text() if p.exists() else ""

def function(source, marker):
    start = source.index(marker)
    brace = source.index("{", start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"

fastio = read("src/fastio.c")
cache = read("src/cache.c")
threads = read("src/threads.c")
flush = threads if "static void do_flush(" in threads else read("src/flushthread.c")
pieces = [function(fastio, "static BOOLEAN __stdcall fast_io_write("),
          function(cache, "static BOOLEAN __stdcall acquire_for_lazy_write("),
          function(cache, "static void __stdcall release_from_lazy_write("),
          function(flush, "static void do_flush(")]
(out / "throttle.inc").write_text("\n".join(pieces))
binary = out / "throttle_test"
flags = ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"] if args.sanitize else []
subprocess.run(["gcc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror",
                "-O1", "-g", "-pthread", *flags, "-I", str(out),
                str(Path(__file__).with_name("throttle_test.c")), "-o", str(binary)], check=True)
cases = ["throttled", "throttle-closes", "unthrottled"]
failed = 0
for case in cases:
    try:
        result = subprocess.run([str(binary), case], timeout=10)
        failed += result.returncode != 0
    except subprocess.TimeoutExpired:
        print(f"FAIL {case}: timed out", flush=True)
        failed += 1
print(f"{len(cases) - failed}/{len(cases)} scenarios passed", flush=True)
raise SystemExit(bool(failed))
