"""Synthetic end-to-end evidence checks for the fixed range benchmark."""

import json
import math
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import assess

HASH = "d384285c86b3172e77ebd28acd7620abe2c160b3d17b3c9899f8f3bb88ded15a"
DATASETS = {
    "overlap": "04bf6285337dc4e4",
    "before": "216876be5420483f",
    "mixed": "81d93c8af0c82912",
    "u64-stress": "80e85a96e3e34966",
}
ORDERS = ["c-first", "reference-first", "rust-first"]
BACKENDS = {
    "c-first": ["c", "reference", "rust", "baseline"],
    "reference-first": ["reference", "rust", "c", "baseline"],
    "rust-first": ["rust", "c", "reference", "baseline"],
}
PERMUTATIONS = ["CAB", "CBA", "ABC", "BAC", "ACB", "BCA"]


def write(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value), encoding="utf-8")


def fixture(root, phase="calibration", rust_factor=0.97):
    """Write raw Criterion and telemetry files with hand-set stable ratios."""
    runs = []
    order_ids = ORDERS if phase == "calibration" else [ORDERS[ord(letter) - 65]
                                                      for block in PERMUTATIONS for letter in block]
    for index, order in enumerate(order_ids):
        name = f"run-{index + 1:02d}"
        run_root = root / name
        run_root.mkdir(parents=True, exist_ok=True)
        write(run_root / "inputs.json", {
            "seed": "6e5a4b139c20d7f1", "order": order, "backends": BACKENDS[order],
            "datasets": [{"name": k, "cases": 1024, "fnv1a64_le": v}
                         for k, v in DATASETS.items()],
        })
        for dataset in DATASETS:
            for backend in BACKENDS[order]:
                value = {"c": 100.0, "reference": 100.0,
                         "rust": 100.0 * rust_factor, "baseline": 20.0}[backend]
                path = run_root / "range" / dataset / backend / "new"
                x = list(range(1, 101))
                write(path / "sample.json", {"sampling_mode": "Linear", "iters": x,
                                             "times": [v * value for v in x]})
                write(path / "estimates.json", {"slope": {"point_estimate": value,
                    "confidence_interval": {"confidence_level": 0.95,
                                            "lower_bound": value * 0.99,
                                            "upper_bound": value * 1.01}}})
                write(path / "benchmark.json", {"group_id": "range", "function_id": dataset,
                    "value_str": backend, "full_id": f"range/{dataset}/{backend}",
                    "directory_name": f"range/{dataset}/{backend}"})
        runs.append({"name": name, "order": order, "status": "complete", "process_id": 900 + index,
                     "exit_code": 0,
                     "sha256_before": HASH, "sha256_after": HASH,
                     "launch_affinity": {"parent_process_id": 800, "parent_original_mask": 2,
                        "mask": 1, "verified_seconds": .01, "start_seconds": .02,
                        "return_seconds": .2, "parent_restored_mask": 2},
                     "guest_affinity": [{"seconds": .25, "mask": 1}, {"seconds": 2.1, "mask": 1}],
                     "started_monotonic": index * 10 + 1,
                     "ended_monotonic": index * 10 + 4})
    manifest = {"schema": 1, "phase": phase, "session_id": phase + "-001",
                "expected_sha256": HASH, "guest_cpu": 0, "smoke": False,
                "status": "complete", "preflight_file": "preflight.json", "runs": runs,
                "runner_affinity": {"incoming_mask": 3, "steady_mask": 2,
                                    "restored_mask": 3}}
    write(root / "preflight.json", {"schema": 1, "phase": phase,
        "session_id": phase + "-001", "samples": [
            {"seconds": 15 * i, "idle": 960 * i, "kernel": 980 * i,
             "user": 20 * i, "available_bytes": 2 * 1024 ** 3}
            for i in range(5)]})
    if phase == "comparison":
        manifest["calibration_session_id"] = "calibration-001"
    write(root / "manifest.json", manifest)
    trace = []
    for i in range(len(runs) * 10 + 1):
        trace.append({"t": float(i), "phase": phase, "session_id": phase + "-001",
            "pid": 42, "starttime": 1001, "cgroup": "/bench",
            "memory": {"current": 1000000, "events": {"max": 0, "oom": 0,
                "oom_kill": 0, "oom_group_kill": 0},
                       "stat": {"anon": 500000, "file": 500000}},
            "pressure": {"memory": "some avg10=0.00", "cpu": "some avg10=0.00"},
            "rss_bytes": 1000000, "cpu_ticks": i,
            "qmp_cpus": {"0": 43, "1": 44},
            "threads": [{"tid": 43, "role": "CPU 0/KVM", "affinity": [2], "processor": 2,
                         "starttime": 1001, "schedstat": [100, 0, 1]},
                        {"tid": 44, "role": "CPU 1/KVM", "affinity": [4], "processor": 4,
                         "starttime": 1001, "schedstat": [100, 0, 1]},
                        {"tid": 42, "role": "emulator", "affinity": [4], "processor": 4,
                         "starttime": 1001, "schedstat": [100, 0, 1]}],
            "expected_cpu": 2, "sibling_cpus": [3]})
    (root / "host.jsonl").write_text("".join(json.dumps(row) + "\n" for row in trace))
    write(root / "host-result.json", {"schema": 1, "phase": phase,
        "session_id": phase + "-001", "status": "complete", "exit_code": 0,
        "started_monotonic": 0, "ended_monotonic": len(runs) * 10,
        "monitor_affinity": [4, 5]})
    return manifest


class EvidenceTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "calibration"
        self.manifest = fixture(self.root)

    def check_invalid(self, expected):
        with self.assertRaisesRegex(assess.InvalidEvidence, expected):
            assess.assess(self.root, HASH)

    def save_manifest(self):
        write(self.root / "manifest.json", self.manifest)

    def trace(self):
        return [json.loads(line) for line in (self.root / "host.jsonl").read_text().splitlines()]

    def save_trace(self, rows):
        (self.root / "host.jsonl").write_text("".join(json.dumps(row) + "\n" for row in rows))

    def fresh_trace(self):
        fixture(self.root)
        return self.trace()

    def test_valid_calibration_and_comparison_report(self):
        self.assertEqual(assess.assess(self.root, HASH)["valid"], True)
        comparison = self.root.parent / "comparison"
        fixture(comparison, "comparison")
        report = assess.assess(comparison, HASH, self.root)
        self.assertEqual(report["valid"], True)
        self.assertEqual(len(report["pairs"]), 12)
        self.assertAlmostEqual(report["pairs"]["rust/reference:overlap"]["geometric_ratio"], .97)
        self.assertEqual(report["pairs"]["rust/reference:overlap"]["classification"], "faster")
        self.assertEqual(len(report["pairs"]["rust/reference:overlap"]["blocks"]), 6)

    def test_wrong_hash_and_nonzero_exit_reject_entire_phase(self):
        self.manifest["runs"][1]["sha256_after"] = "0" * 64
        self.save_manifest()
        self.check_invalid("hash")
        self.manifest["runs"][1]["sha256_after"] = HASH
        self.manifest["runs"][1]["exit_code"] = 1
        self.save_manifest()
        self.check_invalid("exit")

    def test_incomplete_run_and_guest_affinity_gap_rejected(self):
        self.manifest["runs"][0]["status"] = "running"
        self.save_manifest(); self.check_invalid("incomplete")
        self.manifest["runs"][0]["status"] = "complete"
        self.manifest["runs"][0]["guest_affinity"] = [
            {"seconds": .1, "mask": 1}, {"seconds": 2.8, "mask": 1}]
        self.save_manifest(); self.check_invalid("guest affinity gap")

    def test_inherited_launch_mask_and_child_observation_required(self):
        run = self.manifest["runs"][0]
        run["launch_affinity"]["mask"] = 3
        self.save_manifest(); self.check_invalid("launch affinity")
        run["launch_affinity"]["mask"] = 1
        run["launch_affinity"]["return_seconds"] = 2.5
        self.save_manifest(); self.check_invalid("first child affinity")
        run["launch_affinity"]["return_seconds"] = .2
        run["launch_affinity"]["parent_restored_mask"] = 3
        self.save_manifest(); self.check_invalid("parent affinity restoration")

    def test_missing_or_wrong_order_run_rejected(self):
        self.manifest["runs"].pop()
        self.save_manifest()
        self.check_invalid("runs")
        self.manifest = fixture(self.root)
        self.manifest["runs"][0]["order"] = "rust-first"
        self.save_manifest()
        self.check_invalid("order")

    def test_corpus_and_backend_registration_rejected(self):
        inputs = self.root / "run-01" / "inputs.json"
        data = json.loads(inputs.read_text())
        data["datasets"][0]["fnv1a64_le"] = "bad"
        write(inputs, data)
        self.check_invalid("fingerprint")
        data["datasets"][0]["fnv1a64_le"] = DATASETS["overlap"]
        data["backends"].reverse()
        write(inputs, data)
        self.check_invalid("backend")

    def test_missing_nonfinite_or_non_linear_samples_rejected(self):
        sample = self.root / "run-01/range/overlap/c/new/sample.json"
        data = json.loads(sample.read_text())
        data["times"].pop()
        write(sample, data)
        self.check_invalid("100")
        fixture(self.root)
        data = json.loads(sample.read_text())
        data["times"][5] = "NaN"
        write(sample, data)
        self.check_invalid("finite")
        fixture(self.root)
        data = json.loads(sample.read_text())
        data["sampling_mode"] = "Flat"
        write(sample, data)
        self.check_invalid("Linear")

    def test_linear_iterations_must_be_exact_multiples(self):
        path = self.root / "run-01/range/overlap/c/new/sample.json"
        data = json.loads(path.read_text())
        data["iters"][50] += .5
        write(path, data)
        self.check_invalid("iteration sequence")

    def test_high_guest_load_is_reported_without_hiding_invalid_evidence(self):
        path = self.root / "preflight.json"
        data = json.loads(path.read_text())
        for index, row in enumerate(data["samples"]):
            row["idle"] = 100 * index  # 90% busy in each interval.
        write(path, data)
        report = assess.assess(self.root, HASH)
        self.assertEqual(report["guest_preflight_busy_fractions"], [.9] * 4)
        data["samples"][2]["idle"] = 99  # A counter reset under high load.
        write(path, data)
        self.check_invalid("counter reset")
        data["samples"][2]["idle"] = 200
        data["samples"][2]["seconds"] = 20
        write(path, data)
        self.check_invalid("interval")
        data["samples"][2]["seconds"] = 30
        data["samples"][2]["available_bytes"] = 512 * 1024 ** 2
        write(path, data)
        self.check_invalid("available memory")
        data["samples"][2]["available_bytes"] = 2 * 1024 ** 3
        data["session_id"] = "other"
        write(path, data)
        self.check_invalid("preflight.*session")

    def test_nonconstant_blocks_use_six_run_t_interval(self):
        values = []
        for block in range(6):
            block_effect = -.03 if block % 2 == 0 else .03
            for letter in PERMUTATIONS[block]:
                order = ORDERS[ord(letter) - 65]
                order_effect = {"c-first": -.002, "reference-first": 0,
                                "rust-first": .002}[order]
                ratio = math.exp(block_effect + order_effect)
                values.append({name: {"rust": {"slope": ratio},
                                      "reference": {"slope": 1},
                                      "c": {"slope": 1}}
                               for name in DATASETS})
        report = assess.comparison_report(values)["rust/reference:overlap"]
        # Alternating +/-0.03 log blocks: mean 0, sample SD=sqrt(6/5)*.03.
        halfwidth = 2.570581835636305 * .03 / math.sqrt(5)
        self.assertAlmostEqual(report["geometric_ratio"], 1)
        self.assertAlmostEqual(report["ci95"][0], math.exp(-halfwidth))
        self.assertAlmostEqual(report["ci95"][1], math.exp(halfwidth))
        self.assertEqual(report["classification"], "inconclusive")
        self.assertAlmostEqual(report["per_order"]["c-first"], math.exp(-.002))
        self.assertAlmostEqual(report["per_order"]["reference-first"], 1)
        self.assertAlmostEqual(report["per_order"]["rust-first"], math.exp(.002))

    def test_exactly_five_percent_guest_load_is_valid(self):
        path = self.root / "preflight.json"
        data = json.loads(path.read_text())
        for index, row in enumerate(data["samples"]):
            row["idle"] = 950 * index
            row["kernel"] = 970 * index
            row["user"] = 30 * index
        write(path, data)
        self.assertEqual(assess.assess(self.root, HASH)["guest_preflight_busy_fractions"],
                         [.05] * 4)

    def test_comparison_reports_both_phase_preflight_loads(self):
        comparison = self.root.parent / "comparison"
        fixture(comparison, "comparison")
        data = json.loads((comparison / "preflight.json").read_text())
        for index, row in enumerate(data["samples"]):
            row["idle"] = 100 * index
        write(comparison / "preflight.json", data)
        report = assess.assess(comparison, HASH, self.root)
        self.assertEqual(report["guest_preflight_busy_fractions"], [.9] * 4)
        self.assertEqual(report["calibration_guest_preflight_busy_fractions"], [.04] * 4)

    def test_classification_boundaries_are_predeclared(self):
        examples = [(.96, .979, "faster"), (.98, 1.02, "equivalent"),
                    (1.021, 1.04, "slower"), (.979, .98, "inconclusive"),
                    (1.02, 1.021, "inconclusive")]
        for lo, hi, expected in examples:
            with self.subTest(lo=lo, hi=hi):
                self.assertEqual(assess.classify(lo, hi), expected)

    def test_stale_slope_and_wide_ci_rejected(self):
        path = self.root / "run-01/range/overlap/c/new/estimates.json"
        data = json.loads(path.read_text())
        data["slope"]["point_estimate"] *= 1.2
        write(path, data)
        self.check_invalid("slope")
        fixture(self.root)
        data = json.loads(path.read_text())
        data["slope"]["confidence_interval"]["upper_bound"] *= 1.2
        write(path, data)
        self.check_invalid("halfwidth")

    def test_drift_and_cross_run_baseline_rejected(self):
        path = self.root / "run-01/range/overlap/c/new/sample.json"
        data = json.loads(path.read_text())
        data["times"][-20:] = [v * 1.2 for v in data["times"][-20:]]
        write(path, data)
        estimate = path.with_name("estimates.json")
        e = json.loads(estimate.read_text())
        x, y = data["iters"], data["times"]
        e["slope"]["point_estimate"] = sum(a*b for a,b in zip(x,y))/sum(a*a for a in x)
        e["slope"]["confidence_interval"] = {"confidence_level": .95,
            "lower_bound": e["slope"]["point_estimate"]*.99,
            "upper_bound": e["slope"]["point_estimate"]*1.01}
        write(estimate, e)
        self.check_invalid("drift")
        fixture(self.root)
        path = self.root / "run-02/range/overlap/baseline/new/sample.json"
        data = json.loads(path.read_text())
        data["times"] = [v * 1.2 for v in data["times"]]
        write(path, data)
        estimate = path.with_name("estimates.json")
        e = json.loads(estimate.read_text()); e["slope"]["point_estimate"] *= 1.2
        lo = e["slope"]["confidence_interval"]; lo["lower_bound"] *= 1.2; lo["upper_bound"] *= 1.2
        write(estimate, e)
        self.check_invalid("baseline")

    def test_host_identity_gap_events_reset_and_affinity_rejected(self):
        rows = self.fresh_trace(); rows[3]["pid"] = 99; self.save_trace(rows)
        self.check_invalid("identity")
        rows = self.fresh_trace(); rows.pop(3); rows.pop(3); rows.pop(3); self.save_trace(rows)
        self.check_invalid("gap")
        rows = self.fresh_trace(); rows[3]["memory"]["events"]["max"] = 1; self.save_trace(rows)
        self.check_invalid("memory event")
        rows = self.fresh_trace(); rows[3]["memory"]["events"]["max"] = -1; self.save_trace(rows)
        self.check_invalid("memory event")
        rows = self.fresh_trace(); rows[3]["threads"][0]["affinity"] = [3]
        rows[3]["threads"][0]["processor"] = 3; self.save_trace(rows)
        self.check_invalid("affinity")

    def test_emulator_thread_birth_and_retirement_are_valid(self):
        rows = self.trace()
        rows[3]["threads"] = [thread for thread in rows[3]["threads"] if thread["tid"] != 42]
        rows[4]["threads"].append({"tid": 45, "role": "emulator", "affinity": [4],
                                    "processor": 4, "starttime": 1002,
                                    "schedstat": [10, 0, 1]})
        self.save_trace(rows)
        self.assertTrue(assess.assess(self.root, HASH)["valid"])

    def test_last_cpu_can_predate_current_affinity(self):
        rows = self.trace()
        for row in rows:
            row["threads"][2]["processor"] = 6
        self.save_trace(rows)
        self.assertTrue(assess.assess(self.root, HASH)["valid"])

    def test_actual_emulator_affinity_and_last_cpu_type_are_checked(self):
        for cpu in (2, 3):
            rows = self.fresh_trace()
            rows[3]["threads"][2]["affinity"].append(cpu)
            self.save_trace(rows)
            self.check_invalid("other QEMU thread affinity")
        for invalid in (-1, "4", None, True):
            rows = self.fresh_trace()
            rows[3]["threads"][2]["processor"] = invalid
            self.save_trace(rows)
            self.check_invalid("host thread telemetry")

    def test_host_command_failure_and_session_mismatch_rejected(self):
        result = self.root / "host-result.json"
        data = json.loads(result.read_text()); data["exit_code"] = 255
        write(result, data)
        self.check_invalid("host command")
        data["exit_code"] = 0; data["session_id"] = "other"
        write(result, data)
        self.check_invalid("host.*session")

    def test_monitor_affinity_must_avoid_benchmark_siblings(self):
        path = self.root / "host-result.json"
        data = json.loads(path.read_text())
        data["monitor_affinity"] = [3, 4]
        write(path, data)
        self.check_invalid("monitor affinity")

    def test_missing_telemetry_and_trace_phase_rejected(self):
        rows = self.trace(); del rows[3]["memory"]["stat"]
        self.save_trace(rows); self.check_invalid("memory telemetry")
        rows = self.fresh_trace(); rows[3]["session_id"] = "other"
        self.save_trace(rows); self.check_invalid("host trace.*session")

    def test_qmp_vcpu_mapping_must_match_thread_roles(self):
        rows = self.trace(); rows[3]["qmp_cpus"]["0"] = 44
        self.save_trace(rows); self.check_invalid("QMP")

    def test_comparison_revalidates_raw_calibration(self):
        comparison = self.root.parent / "comparison"
        fixture(comparison, "comparison")
        self.manifest["runs"][0]["exit_code"] = 1
        self.save_manifest()
        with self.assertRaisesRegex(assess.InvalidEvidence, "calibration"):
            assess.assess(comparison, HASH, self.root)

    def test_baseline_spread_includes_calibration_and_comparison(self):
        comparison = self.root.parent / "comparison"
        fixture(comparison, "comparison")
        for index in range(1, 19):
            path = comparison / f"run-{index:02d}/range/overlap/baseline/new"
            sample = json.loads((path / "sample.json").read_text())
            sample["times"] = [v * 1.125 for v in sample["times"]]
            write(path / "sample.json", sample)
            estimate = json.loads((path / "estimates.json").read_text())
            estimate["slope"]["point_estimate"] *= 1.125
            estimate["slope"]["confidence_interval"]["lower_bound"] *= 1.125
            estimate["slope"]["confidence_interval"]["upper_bound"] *= 1.125
            write(path / "estimates.json", estimate)
        with self.assertRaisesRegex(assess.InvalidEvidence, "baseline"):
            assess.assess(comparison, HASH, self.root)

    def test_trace_must_cover_host_command(self):
        path = self.root / "host-result.json"
        result = json.loads(path.read_text())
        result["ended_monotonic"] = 1000
        write(path, result)
        self.check_invalid("coverage")


if __name__ == "__main__":
    unittest.main()
