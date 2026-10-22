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
    if args.baseline_dir:
        command.extend(["--baseline-dir", args.baseline_dir])


runner = pyperf.Runner(add_cmdline_args=worker_args)
runner.argparser.add_argument("--build-dir", required=True)
runner.argparser.add_argument("--baseline-dir", help="Interleave before/after cases in one suite")
args = runner.parse_args()
root = Path(args.build_dir).resolve()
candidate_provenance = json.loads((root / "provenance.json").read_text())
if not candidate_provenance.get("optimized"):
    raise ValueError("rebuild the replay with --bench before timing it")
runner.metadata.update({"description": "Production get/update/add replay; C interval only",
                        "fixture_setup": "excluded identically before and after",
                        "source_provenance": json.dumps(json.loads((root / "provenance.json").read_text()))})
if args.baseline_dir:
    runner.metadata["baseline_provenance"] = json.dumps(json.loads(
        (Path(args.baseline_dir).resolve() / "provenance.json").read_text()))
for count in (40000, 87000):
    for prefill in (0, 1000, 10000):
        name = f"extents={count},prefill={prefill}"
        if args.baseline_dir:
            baseline = Path(args.baseline_dir).resolve()
            old = json.loads((baseline / "provenance.json").read_text())
            new = json.loads((root / "provenance.json").read_text())
            if not old.get("optimized"):
                raise ValueError("rebuild the baseline with --bench before timing it")
            if old["trace_sha256"] != new["trace_sha256"]:
                raise ValueError("before/after trace hashes differ")
            runner.bench_time_func(name + "/before", replay, str(baseline / "replay"),
                                   str(baseline / f"trace-{count}.txt"), prefill)
            name += "/after"
        runner.bench_time_func(name, replay,
                               str(root / "replay"), str(root / f"trace-{count}.txt"), prefill)
