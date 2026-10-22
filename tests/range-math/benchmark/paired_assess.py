"""Validate interleaved range helper evidence and infer across six blocks."""

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

from assess import (DATASETS, ORDERS, PAIRS, PERMUTATIONS, T95_DF5,
                    InvalidEvidence, classify, load, positive, require, validate_envelope)

PROTOCOL = "interleaved-v2"
SCHEDULE = "splitmix64-fisher-yates-fnv1a64-v1"
LABELS = ["c", "reference", "rust", "baseline-a", "baseline-b"]
ALL_PAIRS = (*PAIRS, ("baseline-a", "baseline-b"))
MASK = (1 << 64) - 1
CALLS_PER_CYCLE = 167772160
# Bound reconstruction work before walking any claimed schedule. These are
# evidence capacity limits, independent of measured durations and ratios.
MAX_RECORDS = 1024
MAX_CYCLES = 65536
CRITERION = {"samples": 10, "warmup_ns": 2000000000, "measurement_ns": 5000000000,
             "confidence_level": .95, "sampling_mode": "Flat"}


def uint(value, label, nonzero=True):
    require(type(value) is int and (0 < value if nonzero else 0 <= value)
            and value <= MASK, f"{label} must be an unsigned 64-bit integer")
    return value


def schedule_digest(order, start, count):
    """Reconstruct label-byte FNV from the documented wrapping SplitMix schedule."""
    rows = [[(label + rotation) % 5 for label in (0, 1, 4, 2, 3)]
            for rotation in range(5)]
    rows += [list(reversed(row)) for row in rows]
    digest = 0xcbf29ce484222325
    for cycle in range(start, start + count):
        state = (0x6e5a4b139c20d7f1 ^ (((order + 1) * 0xd1b54a32d192ed03) & MASK)
                 ^ ((cycle * 0x94d049bb133111eb) & MASK))
        row_ids = list(range(10))
        for index in range(9, 0, -1):
            state = (state + 0x9e3779b97f4a7c15) & MASK
            value = ((state ^ (state >> 30)) * 0xbf58476d1ce4e5b9) & MASK
            value = ((value ^ (value >> 27)) * 0x94d049bb133111eb) & MASK
            other = (value ^ (value >> 31)) % (index + 1)
            row_ids[index], row_ids[other] = row_ids[other], row_ids[index]
        for index in row_ids:
            for label in rows[index]:
                digest = ((digest ^ label) * 0x100000001b3) & MASK
    return f"{digest:016x}"


def validate_inputs(root, order):
    inputs = load(root / "inputs.json")
    require(isinstance(inputs, dict) and inputs.get("protocol") == PROTOCOL,
            "interleaved-v2 protocol required")
    require(inputs.get("mode") == "measurement", "measurement metadata required")
    require(inputs.get("schedule") == SCHEDULE and inputs.get("backends") == LABELS
            and inputs.get("seed") == "6e5a4b139c20d7f1", "paired schedule or label metadata mismatch")
    require(inputs.get("order") == order and type(inputs.get("order_index")) is int
            and inputs["order_index"] == ORDERS.index(order), "paired order metadata mismatch")
    require(all(type(inputs.get(key)) is int and inputs[key] == value for key, value in
                (("batches_per_chunk", 16384), ("cases_per_batch", 1024),
                 ("chunks_per_label_cycle", 10), ("calls_per_label_cycle", CALLS_PER_CYCLE))),
            "unequal or incorrect work metadata")
    require(inputs.get("criterion") == CRITERION, "Criterion configuration metadata mismatch")
    datasets = inputs.get("datasets")
    require(isinstance(datasets, list) and len(datasets) == 4
            and all(isinstance(row, dict) and type(row.get("cases")) is int for row in datasets)
            and [(row.get("name"), row.get("cases"), row.get("fnv1a64_le")) for row in datasets]
            == [(name, 1024, fingerprint) for name, fingerprint in DATASETS.items()],
            "dataset fingerprint or count mismatch")
    probe = inputs.get("timer_probe")
    require(isinstance(probe, dict) and type(probe.get("count")) is int
            and probe["count"] == 10000 and isinstance(probe.get("ns"), list)
            and len(probe["ns"]) == 10000, "10000 raw timer probes required")
    values = [uint(ns, "timer probe duration", nonzero=False) for ns in probe["ns"]]
    p99 = uint(probe.get("p99_ns"), "timer probe p99")
    require(p99 == sorted(values)[9899], "timer probe p99 mismatch")
    return p99


def validate_case(root, dataset, order, p99, pilot):
    path = root / "paired" / dataset / "new"
    meta = load(path / "benchmark.json")
    identity = f"paired/{dataset}"
    require(isinstance(meta, dict) and meta.get("group_id") == "paired"
            and meta.get("function_id") == dataset and meta.get("value_str") is None
            and meta.get("full_id") == identity and meta.get("directory_name") == identity,
            f"paired case identity mismatch: {path}")
    sidecar = load(root / f"paired-{dataset}.json")
    require(isinstance(sidecar, dict) and sidecar.get("protocol") == PROTOCOL
            and sidecar.get("dataset") == dataset and sidecar.get("order") == order,
            f"sidecar identity or protocol mismatch: {path}")
    records = sidecar.get("records")
    require(isinstance(records, list) and len(records) > 10,
            "all warmup records and 10 measured records required")
    require(len(records) <= MAX_RECORDS, "sidecar record capacity exceeded")
    require(all(isinstance(row, dict) for row in records), "sidecar record must be an object")
    require(sum(uint(row.get("iterations"), "sidecar iterations") for row in records)
            <= MAX_CYCLES, "sidecar cycle capacity exceeded")
    sample = load(path / "sample.json")
    require(isinstance(sample, dict) and sample.get("sampling_mode") == "Flat",
            f"Flat samples required: {path}")
    iterations, times = sample.get("iters"), sample.get("times")
    require(isinstance(iterations, list) and isinstance(times, list)
            and len(iterations) == len(times) == 10, "10 Flat samples required")
    require(all(type(value) in (int, float) and math.isfinite(value)
                and value > 0 and value == int(value) and value == iterations[0]
                for value in iterations), "equal positive integer Flat sample iterations required")
    next_cycle = 0
    minima = [MASK] * 5
    for index, row in enumerate(records):
        require(isinstance(row, dict), "sidecar record must be an object")
        count = uint(row.get("iterations"), "sidecar iterations")
        start = uint(row.get("cycle_start"), "sidecar cycle_start", nonzero=False)
        require(start == next_cycle, "sidecar cycle continuity mismatch")
        next_cycle = start + count
        require(next_cycle <= MASK and count * CALLS_PER_CYCLE <= MASK,
                "sidecar cycle or call count integer overflow")
        ns, minimum = row.get("ns"), row.get("min_chunk_ns")
        require(isinstance(ns, list) and isinstance(minimum, list)
                and len(ns) == len(minimum) == 5, "five integer label durations and minima required")
        for label in range(5):
            value = uint(ns[label], "label duration")
            low = uint(minimum[label], "minimum chunk duration")
            require(low * 10 * count <= value, "minimum chunk exceeds label duration")
            minima[label] = min(minima[label], low)
        require(sum(ns) <= MASK, "total duration integer overflow")
        # Verify the final sample binding before potentially expensive reconstruction.
        if index >= len(records) - 10:
            slot = index - (len(records) - 10)
            require(iterations[slot] == count, "sample iterations differ from sidecar")
            positive(times[slot], "sample time")
            require(times[slot] == float(sum(ns)), "sample time differs from sidecar sum")
        require(row.get("schedule_digest") == schedule_digest(ORDERS.index(order), start, count),
                "schedule digest mismatch")
    require(p99 * 200 <= min(minima), "timestamp probe p99 exceeds 0.5% chunk budget")
    estimates = load(path / "estimates.json")
    estimate = estimates.get("mean") if isinstance(estimates, dict) else None
    require(isinstance(estimate, dict), "mean estimate missing")
    point = positive(estimate.get("point_estimate"), "mean estimate")
    mean = statistics.mean(t / n for t, n in zip(times, iterations))
    require(math.isclose(point, mean, rel_tol=1e-12), "stale mean estimate")
    ci = estimate.get("confidence_interval")
    require(isinstance(ci, dict) and ci.get("confidence_level") == .95, "95% mean CI required")
    lower = positive(ci.get("lower_bound"), "mean CI lower")
    upper = positive(ci.get("upper_bound"), "mean CI upper")
    require(lower <= point <= upper, "mean CI excludes point")
    measured = records[-10:]
    totals = [sum(row["ns"][label] for row in measured) for label in range(5)]
    calls = sum(row["iterations"] for row in measured) * CALLS_PER_CYCLE
    ratios, drift = {}, {}
    for numerator, denominator in ALL_PAIRS:
        key = f"{numerator}/{denominator}"
        a, b = LABELS.index(numerator), LABELS.index(denominator)
        ratios[key] = totals[a] / totals[b]
        samples = [row["ns"][a] / row["ns"][b] for row in measured]
        drift[key] = statistics.median(samples[-2:]) / statistics.median(samples[:2])
        require(.90 - 1e-12 <= drift[key] <= 1.10 + 1e-12,
                f"paired ratio drift outside budget: {key}:{dataset}")
    aa = ratios["baseline-a/baseline-b"]
    require(not pilot or .98 - 1e-12 <= aa <= 1.02 + 1e-12,
            f"A/A ratio outside pilot futility budget: {dataset}")
    return {"ratios": ratios, "ratio_drift": drift, "calls_per_label": calls,
            "total_ns": dict(zip(LABELS, totals)),
            "ns_per_call": dict(zip(LABELS, (ns / calls for ns in totals))),
            "min_chunk_ns": dict(zip(LABELS, minima)), "timer_probe_p99_ns": p99,
            "warmup_records": len(records) - 10, "measured_records": 10,
            "criterion_mean_ns": point, "criterion_mean_ci95": [lower, upper]}


def validate_phase(root, expected_sha256, phase=None, host_root=None):
    manifest, runs, trace, preflight = validate_envelope(root, expected_sha256, phase, host_root)
    values = []
    for run_root, order in runs:
        p99 = validate_inputs(run_root, order)
        expected_sidecars = {f"paired-{dataset}.json" for dataset in DATASETS}
        require({path.name for path in run_root.glob("paired-*.json")} == expected_sidecars,
                "missing or extra paired case evidence")
        expected_cases = {f"paired/{dataset}/new/{file}.json" for dataset in DATASETS
                          for file in ("benchmark", "sample", "estimates")}
        actual_cases = {str(path.relative_to(run_root))
                        for file in ("benchmark", "sample", "estimates")
                        for path in run_root.rglob(f"{file}.json") if path.parent.name == "new"}
        require(actual_cases == expected_cases, "missing or extra Criterion case evidence")
        values.append({dataset: validate_case(run_root, dataset, order, p99,
                                             manifest["phase"] == "calibration")
                       for dataset in DATASETS})
    return manifest, values, trace, preflight


def block_report(values, pairs=ALL_PAIRS):
    """Average three process log ratios per block, then infer across six blocks."""
    result = {}
    for dataset in DATASETS:
        for numerator, denominator in pairs:
            key = f"{numerator}/{denominator}"
            blocks = []
            orders = {order: [] for order in ORDERS}
            for block_index, permutation in enumerate(PERMUTATIONS):
                logs = []
                for slot, letter in enumerate(permutation):
                    ratio = values[block_index * 3 + slot][dataset]["ratios"][key]
                    log = math.log(positive(ratio, key))
                    logs.append(log)
                    orders[ORDERS[ord(letter) - 65]].append(log)
                blocks.append(statistics.mean(logs))
            mean = statistics.mean(blocks)
            halfwidth = T95_DF5 * statistics.stdev(blocks) / math.sqrt(6)
            lower, upper = math.exp(mean - halfwidth), math.exp(mean + halfwidth)
            result[f"{key}:{dataset}"] = {
                "geometric_ratio": math.exp(mean), "ci95": [lower, upper],
                "classification": classify(lower, upper),
                "blocks": [math.exp(value) for value in blocks],
                "per_order": {order: math.exp(statistics.mean(logs))
                              for order, logs in orders.items()}}
    return result


def assess(root, expected_sha256, calibration_root=None, host_root=None,
           calibration_host_root=None):
    require(isinstance(expected_sha256, str) and len(expected_sha256) == 64
            and all(c in "0123456789abcdef" for c in expected_sha256),
            "expected SHA-256 must be lowercase hex")
    try:
        return _assess(Path(root), expected_sha256, calibration_root, host_root, calibration_host_root)
    except InvalidEvidence:
        raise
    except (AttributeError, KeyError, TypeError, ValueError, OverflowError) as exc:
        raise InvalidEvidence(f"malformed paired evidence: {exc}") from exc


def _assess(root, expected_sha256, calibration_root, host_root, calibration_host_root):
    manifest, values, trace, preflight = validate_phase(root, expected_sha256, host_root=host_root)
    report = {"valid": True, "protocol": PROTOCOL, "phase": manifest["phase"],
              "session_id": manifest["session_id"], "runs": len(values),
              "host_trace": trace, "guest_preflight_busy_fractions": preflight,
              "run_results": values,
              "notes": ["No baseline, clock-cost subtraction or sample selection.",
                        "The excluded pilot's A/A ratio gate is a futility check, not proof of equivalence.",
                        "Intervals are marginal across workloads and pairs; no familywise claim.",
                        "Six block log ratios are treated as approximately independent and normal; "
                        "adjacent Criterion samples are not independent experiment runs.",
                        "VM elapsed costs include the direct batch loop and observed load; "
                        "A/A cannot exclude every helper-specific interaction or common additive overhead."]}
    spread_values = list(values)
    if manifest["phase"] == "comparison":
        require(calibration_root is not None, "calibration root required")
        calibration, pilot_values, _, pilot_load = validate_phase(
            Path(calibration_root), expected_sha256, "calibration", calibration_host_root)
        require(calibration["session_id"] == manifest.get("calibration_session_id"),
                "calibration session link mismatch")
        report["calibration_session_id"] = calibration["session_id"]
        report["calibration_guest_preflight_busy_fractions"] = pilot_load
        report["pairs"] = block_report(values)
        for dataset in DATASETS:
            interval = report["pairs"][f"baseline-a/baseline-b:{dataset}"]["ci95"]
            require(.98 <= interval[0] <= interval[1] <= 1.02,
                    f"A/A 95% CI outside equivalence band: {dataset}")
        spread_values += pilot_values
    report["baseline_spread"] = {dataset: {
        label: max(run[dataset]["ns_per_call"][label] for run in spread_values)
        / min(run[dataset]["ns_per_call"][label] for run in spread_values)
        for label in ("baseline-a", "baseline-b")} for dataset in DATASETS}
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--expected-sha256", required=True)
    parser.add_argument("--calibration-root", type=Path)
    parser.add_argument("--host-root", type=Path)
    parser.add_argument("--calibration-host-root", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(assess(args.root, args.expected_sha256, args.calibration_root,
                                args.host_root, args.calibration_host_root), indent=2, sort_keys=True))
    except InvalidEvidence as exc:
        print(json.dumps({"valid": False, "protocol": PROTOCOL, "error": str(exc)}), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
