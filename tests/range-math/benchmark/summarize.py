"""Normalize Criterion batch estimates to calls; keep baseline raw."""

import argparse
import json
from pathlib import Path

DATASETS = ("overlap", "before", "mixed", "u64-stress")
CALLS_PER_BATCH = 1024
REFERENCE_ORDERS = {
    "c-first": ["c", "reference", "rust", "baseline"],
    "reference-first": ["reference", "rust", "c", "baseline"],
    "rust-first": ["rust", "c", "reference", "baseline"],
}


def _reference_metadata(root, expected_order=None, datasets=DATASETS):
    inputs = json.loads((Path(root) / "inputs.json").read_text(encoding="utf-8"))
    order = inputs.get("order")
    if order not in REFERENCE_ORDERS or (expected_order and order != expected_order):
        raise ValueError(f"reference run order mismatch: expected {expected_order}, got {order}")
    if inputs.get("backends") != REFERENCE_ORDERS[order]:
        raise ValueError(f"reference run backend order mismatch in {root}")
    fingerprints = {item["name"]: (item["cases"], item["fnv1a64_le"])
                    for item in inputs["datasets"]}
    if any(fingerprints.get(name, (None,))[0] != CALLS_PER_BATCH for name in datasets):
        raise ValueError(f"reference run datasets differ in {root}")
    return inputs["seed"], tuple((name, fingerprints[name]) for name in datasets)


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


def summarize_order(root, datasets=DATASETS, reference=False, expected_order=None):
    root = Path(root)
    if reference:
        _reference_metadata(root, expected_order, datasets)
    summary = {}
    for dataset in datasets:
        names = ("c", "reference", "rust", "baseline") if reference else ("c", "rust", "baseline")
        backends = {name: _estimate(root, dataset, name) for name in names}
        backends["rust_over_c_point"] = (
            backends["rust"]["ns_per_call"] / backends["c"]["ns_per_call"])
        if reference:
            backends["rust_over_reference_point"] = (
                backends["rust"]["ns_per_call"] / backends["reference"]["ns_per_call"])
            backends["reference_over_c_point"] = (
                backends["reference"]["ns_per_call"] / backends["c"]["ns_per_call"])
        summary[dataset] = backends
    return summary


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("c_first", type=Path)
    parser.add_argument("rust_first", type=Path)
    parser.add_argument("reference_first", type=Path, nargs="?")
    args = parser.parse_args()
    reference = args.reference_first is not None
    if reference:
        metadata = [
            _reference_metadata(args.c_first, "c-first"),
            _reference_metadata(args.rust_first, "rust-first"),
            _reference_metadata(args.reference_first, "reference-first"),
        ]
        if len(set(metadata)) != 1:
            raise ValueError("reference runs have different seeds or dataset fingerprints")
    orders = {
        "c-first": summarize_order(args.c_first, reference=reference,
                                   expected_order="c-first" if reference else None),
        "rust-first": summarize_order(args.rust_first, reference=reference,
                                      expected_order="rust-first" if reference else None),
    }
    if reference:
        orders["reference-first"] = summarize_order(args.reference_first, reference=True,
                                                    expected_order="reference-first")
    result = {
        "units": "nanoseconds per call",
        "calls_per_batch": CALLS_PER_BATCH,
        "orders": orders,
    }
    print(json.dumps(result, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
