#!/usr/bin/env python3
"""Use pyperf workers to measure the C replay interval, excluding fixture setup."""
import subprocess
import json
from pathlib import Path
import pyperf


def replay(loops, binary, trace, prefill):
    total = 0.0
    for _ in range(loops):
        result = subprocess.run([binary, trace, str(prefill)], text=True,
                                capture_output=True, check=True)
        total += float(result.stdout)
    return total


def worker_args(command, args):
    command.extend(["--build-dir", args.build_dir])


runner = pyperf.Runner(add_cmdline_args=worker_args)
runner.argparser.add_argument("--build-dir", required=True)
args = runner.parse_args()
root = Path(args.build_dir).resolve()
runner.metadata.update({"description": "Production get/update/add replay; C interval only",
                        "fixture_setup": "excluded identically before and after",
                        "source_provenance": json.dumps(json.loads((root / "provenance.json").read_text()))})
for count in (40000, 87000):
    for prefill in (0, 1000, 10000):
        runner.bench_time_func(f"extents={count},prefill={prefill}", replay,
                               str(root / "replay"), str(root / f"trace-{count}.txt"), prefill)
