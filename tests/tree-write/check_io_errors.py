#!/usr/bin/env python3
"""Fault-inject actual metadata write orchestration and stripe cleanup on a host.

Only Windows allocation, synchronization and device I/O boundaries are replaced.
The complete production functions and context/stripe types are extracted verbatim.
"""
import argparse
import hashlib
import json
from pathlib import Path
import resource
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument("repo", type=Path)
parser.add_argument("--build-dir", type=Path, required=True)
parser.add_argument("--revision")
parser.add_argument("--init", choices=["pattern", "zero"], default="pattern")
parser.add_argument("--sanitize", action="store_true")
args = parser.parse_args()
repo, out = args.repo.resolve(), args.build_dir.resolve()
out.mkdir(parents=True, exist_ok=True)
here = Path(__file__).resolve().parent


def read(path):
    if args.revision:
        return subprocess.run(["git", "-C", str(repo), "show", f"{args.revision}:{path}"],
                              check=True, capture_output=True, text=True).stdout
    return (repo / path).read_text()


def function(source, marker):
    start = source.index(marker)
    end, depth = source.index("{", start) + 1, 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end] + "\n"


source, cleanup, header = read("src/tree-write.c"), read("src/write.c"), read("src/btrfs_drv.h")
start = header.index("enum write_data_status {")
end = header.index("} tree_write;", start) + len("} tree_write;")
(out / "io_types.inc").write_text(header[start:end] + "\n")
(out / "do_tree_writes.inc").write_text(function(source, "NTSTATUS do_tree_writes("))
(out / "free_write_data_stripes.inc").write_text(function(cleanup, "void free_write_data_stripes("))
callee = function(cleanup, "NTSTATUS write_data(")
(out / "write_data_end.inc").write_text(callee[callee.index("\nend:\n"):callee.index("\nprepare_failed:\n")])
flags = ["-std=c11", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
         f"-ftrivial-auto-var-init={args.init}", f"-DPOOL_PATTERN={int(args.init == 'pattern')}"]
if args.sanitize:
    flags += ["-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie"]
binary = out / "io-errors-test"
subprocess.run(["gcc", *flags, "-I", str(out), str(here / "io_errors_test.c"), "-o", str(binary)], check=True)
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
cases = subprocess.check_output([str(binary), "--list"], text=True).splitlines()
results = []
for case in cases:
    run = subprocess.run([str(binary), case], capture_output=True, text=True, timeout=10)
    (out / f"{case}.log").write_text(run.stdout + run.stderr)
    results.append(dict(case=case, passed=run.returncode == 0, exit_code=run.returncode))
    print(f"{'PASS' if run.returncode == 0 else 'FAIL'} {case}", flush=True)
report = dict(cases=results, passed=sum(r["passed"] for r in results), total=len(results),
              revision=args.revision or "working-tree", initialization=args.init, sanitizer=args.sanitize,
              tree_write_sha256=hashlib.sha256(source.encode()).hexdigest(),
              write_sha256=hashlib.sha256(cleanup.encode()).hexdigest())
(out / "results.json").write_text(json.dumps(report, indent=2) + "\n")
print(f"{report['passed']}/{report['total']} scenarios passed ({args.init})", flush=True)
raise SystemExit(report["passed"] != report["total"])
