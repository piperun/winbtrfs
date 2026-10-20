"""Normalize Criterion batch estimates to calls; keep baseline raw."""

import argparse
import json
from pathlib import Path

DATASETS = ("overlap", "before", "mixed", "u64-stress")
CALLS_PER_BATCH = 1024


def _estimate(root, dataset, backend):
    path = root / "range" / dataset / backend / "new" / "estimates.json"
    estimates = json.loads(path.read_text(encoding="utf-8"))
    estimator = "slope" if estimates.get("slope") else "mean"
    value = estimates[estimator]
    interval = value["confidence_interval"]
    return {
        "estimator": estimator,
        "ns_per_call": value["point_estimate"] / CALLS_PER_BATCH,
        "confidence_level": interval["confidence_level"],
        "ci_ns_per_call": [
            interval["lower_bound"] / CALLS_PER_BATCH,
            interval["upper_bound"] / CALLS_PER_BATCH,
        ],
    }


def summarize_order(root, datasets=DATASETS):
    root = Path(root)
    summary = {}
    for dataset in datasets:
        backends = {name: _estimate(root, dataset, name)
                    for name in ("c", "rust", "baseline")}
        backends["rust_over_c_point"] = (
            backends["rust"]["ns_per_call"] / backends["c"]["ns_per_call"])
        summary[dataset] = backends
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("c_first", type=Path)
    parser.add_argument("rust_first", type=Path)
    args = parser.parse_args()
    result = {
        "units": "nanoseconds per call",
        "calls_per_batch": CALLS_PER_BATCH,
        "orders": {
            "c-first": summarize_order(args.c_first),
            "rust-first": summarize_order(args.rust_first),
        },
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
