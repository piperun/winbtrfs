"""Validate complete range benchmark phases and compare independent blocks."""

import argparse
import json
import math
import statistics
import sys
from pathlib import Path

DATASETS = {"overlap": "04bf6285337dc4e4", "before": "216876be5420483f",
            "mixed": "81d93c8af0c82912", "u64-stress": "80e85a96e3e34966"}
ORDERS = ("c-first", "reference-first", "rust-first")
PERMUTATIONS = ("CAB", "CBA", "ABC", "BAC", "ACB", "BCA")
BACKENDS = {"c-first": ["c", "reference", "rust", "baseline"],
            "reference-first": ["reference", "rust", "c", "baseline"],
            "rust-first": ["rust", "c", "reference", "baseline"]}
PAIRS = (("rust", "reference"), ("rust", "c"), ("reference", "c"))
T95_DF5 = 2.570581835636305


class InvalidEvidence(ValueError):
    """The entire phase cannot support a performance conclusion."""


def require(condition, message):
    if not condition:
        raise InvalidEvidence(message)


def load(path):
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        raise InvalidEvidence(f"missing or malformed {path}: {exc}") from exc


def positive(value, label):
    require(type(value) in (int, float) and math.isfinite(value) and value > 0,
            f"{label} must be positive finite")
    return value


def validate_trace(path, phase, session_id):
    try:
        rows = [json.loads(line) for line in path.read_text(encoding="utf-8").splitlines()]
    except (OSError, ValueError) as exc:
        raise InvalidEvidence(f"host trace missing or malformed: {exc}") from exc
    require(len(rows) >= 2, "host trace has insufficient coverage")
    first = rows[0]
    identity = (first.get("pid"), first.get("starttime"), first.get("cgroup"))
    require(all(identity), "host identity missing")
    expected = first.get("expected_cpu")
    siblings = first.get("sibling_cpus")
    require(type(expected) is int and isinstance(siblings, list) and
            all(type(cpu) is int and cpu != expected for cpu in siblings),
            "host topology missing")
    previous_t = None
    previous_events = None
    previous_cpu_ticks = None
    previous_threads = None
    original_qmp = None
    for row in rows:
        require(row.get("phase") == phase and row.get("session_id") == session_id,
                "host trace phase or session identity mismatch")
        require((row.get("pid"), row.get("starttime"), row.get("cgroup")) == identity,
                "host identity changed")
        t = row.get("t")
        require(type(t) in (int, float) and math.isfinite(t), "invalid host timestamp")
        if previous_t is not None:
            require(0 < t - previous_t <= 3, "host trace gap exceeds 3 seconds")
        previous_t = t
        require(row.get("expected_cpu") == expected and row.get("sibling_cpus") == siblings,
                "host topology changed")
        memory = row.get("memory")
        require(isinstance(memory, dict) and positive(memory.get("current"), "memory.current")
                and isinstance(memory.get("stat"), dict)
                and all(type(memory["stat"].get(k)) is int and memory["stat"][k] >= 0
                        for k in ("anon", "file")), "memory telemetry missing")
        events = memory.get("events", {})
        require(all(type(events.get(k)) is int and events[k] >= 0
                    for k in ("max", "oom", "oom_kill", "oom_group_kill")),
                "memory event telemetry missing")
        if previous_events:
            require(all(events[k] == previous_events[k]
                        for k in ("max", "oom", "oom_kill", "oom_group_kill")),
                    "memory event increased or reset")
        previous_events = events
        require(isinstance(row.get("pressure"), dict)
                and all(isinstance(row["pressure"].get(k), str) and row["pressure"][k]
                        for k in ("memory", "cpu"))
                and positive(row.get("rss_bytes"), "RSS")
                and type(row.get("cpu_ticks")) is int and row["cpu_ticks"] >= 0,
                "host telemetry missing")
        if previous_cpu_ticks is not None:
            require(row["cpu_ticks"] >= previous_cpu_ticks, "host CPU counter reset")
        previous_cpu_ticks = row["cpu_ticks"]
        threads = row.get("threads")
        require(isinstance(threads, list) and threads, "host thread telemetry missing")
        qmp = row.get("qmp_cpus")
        require(isinstance(qmp, dict) and set(qmp) == {"0", "1"}
                and all(type(tid) is int and tid > 0 for tid in qmp.values())
                and qmp["0"] != qmp["1"], "QMP vCPU mapping missing")
        if original_qmp is not None:
            require(qmp == original_qmp, "QMP vCPU mapping changed")
        original_qmp = qmp
        seen = set()
        current = {}
        benchmark = []
        for thread in threads:
            tid = thread.get("tid")
            affinity = thread.get("affinity")
            role = thread.get("role")
            # /proc stat records the last CPU used, possibly before a dormant
            # thread's affinity changed; only its allowed mask constrains it.
            require(type(tid) is int and tid not in seen and isinstance(role, str)
                    and isinstance(affinity, list) and affinity
                    and all(type(cpu) is int for cpu in affinity)
                    and type(thread.get("processor")) is int
                    and thread["processor"] >= 0,
                    "host thread telemetry missing")
            require(type(thread.get("starttime")) is int and thread["starttime"] > 0
                    and isinstance(thread.get("schedstat"), list)
                    and len(thread["schedstat"]) == 3
                    and all(type(v) is int and v >= 0 for v in thread["schedstat"]),
                    "host thread schedule telemetry missing")
            seen.add(tid)
            current[tid] = (role, tuple(affinity), thread["starttime"])
            if role == "CPU 0/KVM":
                benchmark.append(thread)
                require(affinity == [expected], "benchmark vCPU affinity changed")
            else:
                require(not {expected, *siblings}.intersection(affinity),
                        "other QEMU thread affinity includes benchmark core or sibling")
        require(len(benchmark) == 1, "benchmark vCPU identity missing or duplicated")
        require(benchmark[0]["tid"] == qmp["0"]
                and any(thread["tid"] == qmp["1"] and thread["role"] == "CPU 1/KVM"
                        for thread in threads), "QMP vCPU mapping disagrees with thread roles")
        if previous_threads is not None:
            for vcpu_tid in qmp.values():
                require(vcpu_tid in previous_threads and vcpu_tid in current
                        and current[vcpu_tid] == previous_threads[vcpu_tid],
                        "QMP vCPU identity or affinity changed")
            for tid in previous_threads.keys() & current.keys():
                before, after = previous_threads[tid], current[tid]
                if before[2] == after[2]:
                    require(before == after,
                            "surviving host thread role or affinity changed")
        previous_threads = current
    return {"samples": len(rows), "seconds": rows[-1]["t"] - rows[0]["t"],
            "host_cpu": expected, "sibling_cpus": siblings,
            "first_t": rows[0]["t"], "last_t": rows[-1]["t"]}


def validate_case(root, dataset, backend):
    path = root / "range" / dataset / backend / "new"
    meta = load(path / "benchmark.json")
    full_id = f"range/{dataset}/{backend}"
    require(meta.get("full_id") == full_id and meta.get("directory_name") == full_id
            and meta.get("group_id") == "range" and meta.get("function_id") == dataset
            and meta.get("value_str") == backend, f"benchmark identity mismatch: {path}")
    sample = load(path / "sample.json")
    require(sample.get("sampling_mode") == "Linear", f"Linear sampling required: {path}")
    x, y = sample.get("iters"), sample.get("times")
    require(isinstance(x, list) and isinstance(y, list)
            and len(x) == len(y) == 100, f"100 samples required: {path}")
    for a, b in zip(x, y):
        positive(a, "iterations")
        positive(b, "sample time")
    require(type(x[0]) in (int, float) and x[0] == int(x[0])
            and all(value == x[0] * (index + 1) for index, value in enumerate(x)),
            f"Linear iteration sequence is not exact multiples: {path}")
    slope = sum(a * b for a, b in zip(x, y)) / sum(a * a for a in x)
    estimates = load(path / "estimates.json")
    estimate = estimates.get("slope")
    require(isinstance(estimate, dict), f"slope estimate missing: {path}")
    point = positive(estimate.get("point_estimate"), "slope")
    require(abs(point / slope - 1) <= 1e-9, f"stale slope estimate: {path}")
    ci = estimate.get("confidence_interval")
    require(isinstance(ci, dict) and ci.get("confidence_level") == .95,
            f"95% slope CI required: {path}")
    lower = positive(ci.get("lower_bound"), "CI lower")
    upper = positive(ci.get("upper_bound"), "CI upper")
    require(lower <= point <= upper, f"CI excludes point estimate: {path}")
    require((upper - lower) / (2 * point) <= .05 + 1e-12,
            f"CI halfwidth exceeds 5%: {path}")
    per_iteration = [b / a for a, b in zip(x, y)]
    drift = statistics.median(per_iteration[-20:]) / statistics.median(per_iteration[:20])
    require(.90 - 1e-12 <= drift <= 1.10 + 1e-12,
            f"within-run drift outside budget: {path}")
    return {"slope": point, "ci": [lower, upper], "drift": drift}


def validate_preflight(root, phase, session_id):
    evidence = load(root / "preflight.json")
    require(evidence.get("schema") == 1 and evidence.get("phase") == phase
            and evidence.get("session_id") == session_id,
            "guest preflight phase or session mismatch")
    samples = evidence.get("samples")
    require(isinstance(samples, list) and len(samples) == 5,
            "guest preflight requires five raw snapshots")
    previous = None
    loads = []
    for row in samples:
        require(isinstance(row, dict), "guest preflight sample malformed")
        require(type(row.get("seconds")) in (int, float)
                and math.isfinite(row["seconds"]) and row["seconds"] >= 0
                and all(type(row.get(k)) is int and row[k] >= 0
                        for k in ("idle", "kernel", "user"))
                and type(row.get("available_bytes")) is int,
                "guest preflight raw counters missing")
        require(row["available_bytes"] >= 1024 ** 3,
                "guest available memory below 1 GiB")
        if previous is not None:
            interval = row["seconds"] - previous["seconds"]
            require(14 <= interval <= 18, "guest preflight interval is not 15 seconds")
            idle = row["idle"] - previous["idle"]
            kernel = row["kernel"] - previous["kernel"]
            user = row["user"] - previous["user"]
            total = kernel + user
            require(0 <= idle <= kernel and kernel >= 0 and user >= 0 and total > 0,
                    "guest preflight counter reset or invalid")
            busy_ticks = total - idle
            loads.append(busy_ticks / total)
        previous = row
    return loads


def validate_envelope(root, expected_sha256, expected_phase=None, host_root=None):
    root = Path(root)
    manifest = load(root / "manifest.json")
    phase = manifest.get("phase")
    require(phase in ("calibration", "comparison") and
            (expected_phase is None or phase == expected_phase), "phase mismatch")
    require(manifest.get("schema") == 1 and manifest.get("status") == "complete"
            and isinstance(manifest.get("session_id"), str) and manifest["session_id"]
            and manifest.get("smoke") is False, "incomplete or smoke phase")
    require(manifest.get("expected_sha256") == expected_sha256,
            "phase expected hash mismatch")
    require(manifest.get("guest_cpu") == 0, "guest benchmark CPU must be zero")
    runner_affinity = manifest.get("runner_affinity")
    require(isinstance(runner_affinity, dict)
            and type(runner_affinity.get("incoming_mask")) is int
            and runner_affinity["incoming_mask"] > 0
            and runner_affinity.get("steady_mask") == 2
            and runner_affinity.get("restored_mask") == runner_affinity["incoming_mask"],
            "runner steady or restored affinity invalid")
    require(manifest.get("preflight_file") == "preflight.json",
            "guest preflight file link missing")
    preflight_loads = validate_preflight(root, phase, manifest["session_id"])
    runs = manifest.get("runs")
    expected_orders = list(ORDERS) if phase == "calibration" else [
        ORDERS[ord(letter) - 65] for block in PERMUTATIONS for letter in block]
    require(isinstance(runs, list) and len(runs) == len(expected_orders),
            f"wrong number of {phase} runs")
    host_root = Path(host_root) if host_root is not None else root
    host_result = load(host_root / "host-result.json")
    require(host_result.get("phase") == phase and
            host_result.get("session_id") == manifest["session_id"],
            "host command phase or session mismatch")
    require(host_result.get("schema") == 1 and host_result.get("status") == "complete"
            and host_result.get("exit_code") == 0, "host command failed or incomplete")
    trace = validate_trace(host_root / "host.jsonl", phase, manifest["session_id"])
    monitor_affinity = host_result.get("monitor_affinity")
    require(isinstance(monitor_affinity, list) and monitor_affinity
            and all(type(cpu) is int for cpu in monitor_affinity)
            and not {trace["host_cpu"], *trace["sibling_cpus"]}.intersection(monitor_affinity),
            "monitor affinity includes benchmark core or sibling")
    start = host_result.get("started_monotonic")
    end = host_result.get("ended_monotonic")
    require(type(start) in (int, float) and type(end) in (int, float)
            and start <= trace["first_t"] <= start + 3
            and end - 3 <= trace["last_t"] <= end,
            "host trace coverage does not enclose command")
    names = set()
    run_paths = []
    for index, (run, expected_order) in enumerate(zip(runs, expected_orders), 1):
        require(isinstance(run, dict), "run metadata missing")
        name = f"run-{index:02d}"
        require(run.get("name") == name and name not in names, "run identity mismatch")
        names.add(name)
        require(run.get("order") == expected_order, "run order mismatch")
        require(run.get("status") == "complete" and
                type(run.get("process_id")) is int and run["process_id"] > 0,
                f"run incomplete or process identity missing: {name}")
        require(run.get("exit_code") == 0, f"run exit missing or nonzero: {name}")
        require(run.get("sha256_before") == expected_sha256
                and run.get("sha256_after") == expected_sha256,
                f"executable hash mismatch: {name}")
        start, end = run.get("started_monotonic"), run.get("ended_monotonic")
        require(type(start) in (int, float) and type(end) in (int, float)
                and 0 <= start < end, f"run timing missing: {name}")
        launch = run.get("launch_affinity")
        require(isinstance(launch, dict)
                and type(launch.get("parent_process_id")) is int
                and launch["parent_process_id"] > 0
                and launch.get("parent_original_mask") == 2
                and launch.get("mask") == 1
                and launch.get("parent_restored_mask") == 2
                and all(type(launch.get(k)) in (int, float)
                        for k in ("verified_seconds", "start_seconds", "return_seconds"))
                and 0 <= launch["verified_seconds"] <= launch["start_seconds"]
                <= launch["verified_seconds"] + 1
                and launch["start_seconds"] <= launch["return_seconds"],
                f"launch affinity or parent affinity restoration invalid: {name}")
        guest = run.get("guest_affinity")
        require(isinstance(guest, list) and len(guest) >= 2
                and launch["return_seconds"] <= guest[0].get("seconds", -1)
                <= launch["return_seconds"] + 1
                and all(type(row.get("seconds")) in (int, float)
                        and type(row.get("mask")) is int and row["mask"] == 1
                        for row in guest)
                and all(0 < guest[i]["seconds"] - guest[i - 1]["seconds"] <= 2
                        for i in range(1, len(guest)))
                and guest[-1]["seconds"] >= end - start + launch["start_seconds"] - 1.5,
                f"first child affinity or guest affinity gap, missing sample or changed mask: {name}")
        run_root = root / name
        run_paths.append((run_root, expected_order))
    return manifest, run_paths, trace, preflight_loads


def validate_phase(root, expected_sha256, expected_phase=None, host_root=None):
    manifest, run_paths, trace, preflight_loads = validate_envelope(
        root, expected_sha256, expected_phase, host_root)
    values = []
    baselines = {dataset: [] for dataset in DATASETS}
    for run_root, expected_order in run_paths:
        name = run_root.name
        inputs = load(run_root / "inputs.json")
        require(inputs.get("order") == expected_order and
                inputs.get("backends") == BACKENDS[expected_order],
                f"backend registration or order mismatch: {name}")
        require(inputs.get("seed") == "6e5a4b139c20d7f1", f"seed mismatch: {name}")
        datasets = inputs.get("datasets")
        require(isinstance(datasets, list) and len(datasets) == 4 and
                [(d.get("name"), d.get("cases"), d.get("fnv1a64_le")) for d in datasets]
                == [(k, 1024, v) for k, v in DATASETS.items()],
                f"dataset fingerprint or count mismatch: {name}")
        case_values = {}
        for dataset in DATASETS:
            case_values[dataset] = {}
            for backend in BACKENDS[expected_order]:
                case_values[dataset][backend] = validate_case(run_root, dataset, backend)
            baselines[dataset].append(case_values[dataset]["baseline"]["slope"])
        values.append(case_values)
    for dataset, points in baselines.items():
        require(max(points) / min(points) <= 1.10 + 1e-12,
                f"cross-run baseline spread exceeds 10%: {dataset}")
    return manifest, values, trace, baselines, preflight_loads


def classify(lo, hi):
    if hi < .98:
        return "faster"
    if lo > 1.02:
        return "slower"
    if .98 <= lo and hi <= 1.02:
        return "equivalent"
    return "inconclusive"


def comparison_report(values):
    pairs = {}
    for dataset in DATASETS:
        for numerator, denominator in PAIRS:
            blocks = []
            orders = {name: [] for name in ORDERS}
            for block_index in range(6):
                ratios = []
                for slot in range(3):
                    index = block_index * 3 + slot
                    ratio = (values[index][dataset][numerator]["slope"] /
                             values[index][dataset][denominator]["slope"])
                    ratios.append(ratio)
                    orders[ORDERS[ord(PERMUTATIONS[block_index][slot]) - 65]].append(ratio)
                blocks.append(sum(math.log(ratio) for ratio in ratios) / 3)
            mean = statistics.mean(blocks)
            halfwidth = T95_DF5 * statistics.stdev(blocks) / math.sqrt(6)
            lo, hi = math.exp(mean - halfwidth), math.exp(mean + halfwidth)
            key = f"{numerator}/{denominator}:{dataset}"
            pairs[key] = {"geometric_ratio": math.exp(mean), "ci95": [lo, hi],
                          "classification": classify(lo, hi),
                          "blocks": [math.exp(v) for v in blocks],
                          "per_order": {name: math.exp(statistics.mean(map(math.log, ratios)))
                                        for name, ratios in orders.items()}}
    return pairs


def assess(root, expected_sha256, calibration_root=None, host_root=None,
           calibration_host_root=None):
    require(len(expected_sha256) == 64 and all(c in "0123456789abcdef" for c in expected_sha256),
            "expected SHA-256 must be lowercase hex")
    manifest, values, trace, baselines, preflight_loads = validate_phase(
        root, expected_sha256, host_root=host_root)
    report = {"valid": True, "phase": manifest["phase"],
              "session_id": manifest["session_id"], "runs": len(values),
              "host_trace": trace,
              "guest_preflight_busy_fractions": preflight_loads,
              "notes": ["No baseline subtraction or normalization.",
                        "Performance estimates describe this VM under its observed load, "
                        "not isolated-hardware performance.",
                        "Intervals are marginal across 12 workload/pair comparisons.",
                        "Six block log-ratio estimates are treated as approximately independent and normal; "
                        "Criterion samples within a run are not independent experiment runs."]}
    if manifest["phase"] == "comparison":
        require(calibration_root is not None, "calibration root required")
        try:
            calibration, _, _, calibration_baselines, calibration_loads = validate_phase(
                calibration_root, expected_sha256, "calibration", calibration_host_root)
        except InvalidEvidence as exc:
            raise InvalidEvidence(f"calibration invalid: {exc}") from exc
        require(calibration["session_id"] == manifest.get("calibration_session_id"),
                "calibration session link mismatch")
        for dataset in DATASETS:
            combined = calibration_baselines[dataset] + baselines[dataset]
            require(max(combined) / min(combined) <= 1.10 + 1e-12,
                    f"cross-phase baseline spread exceeds 10%: {dataset}")
        report["calibration_session_id"] = calibration["session_id"]
        report["calibration_guest_preflight_busy_fractions"] = calibration_loads
        report["pairs"] = comparison_report(values)
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
                                args.host_root, args.calibration_host_root),
                         indent=2, sort_keys=True))
    except InvalidEvidence as exc:
        print(json.dumps({"valid": False, "error": str(exc)}), file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
