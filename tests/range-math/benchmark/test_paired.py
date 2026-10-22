"""Raw-evidence regression tests for the interleaved experiment."""

import json
import math
import shutil
import statistics
import tempfile
import unittest
from pathlib import Path

import assess
import paired_assess as paired
from test_assess import HASH, DATASETS, ORDERS, fixture, write

LABELS = ["c", "reference", "rust", "baseline-a", "baseline-b"]
MASK = (1 << 64) - 1


def schedule_digest(order, start, count):
    """Independent, literal-row oracle for the serialized schedule contract."""
    rows = [[(v + rotation) % 5 for v in (0, 1, 4, 2, 3)] for rotation in range(5)]
    rows += [list(reversed(row)) for row in rows]
    digest = 0xcbf29ce484222325
    for cycle in range(start, start + count):
        state = (0x6e5a4b139c20d7f1 ^ (((order + 1) * 0xd1b54a32d192ed03) & MASK)
                 ^ ((cycle * 0x94d049bb133111eb) & MASK))
        permutation = list(range(10))
        for index in range(9, 0, -1):
            state = (state + 0x9e3779b97f4a7c15) & MASK
            value = ((state ^ (state >> 30)) * 0xbf58476d1ce4e5b9) & MASK
            value = ((value ^ (value >> 27)) * 0x94d049bb133111eb) & MASK
            other = (value ^ (value >> 31)) % (index + 1)
            permutation[index], permutation[other] = permutation[other], permutation[index]
        for index in permutation:
            for label in rows[index]:
                digest = ((digest ^ label) * 0x100000001b3) & MASK
    return f"{digest:016x}"


def save_case(run_root, dataset, records):
    order = json.loads((run_root / "inputs.json").read_text())["order"]
    write(run_root / f"paired-{dataset}.json", {
        "protocol": "interleaved-v2", "dataset": dataset, "order": order, "records": records})
    path = run_root / "paired" / dataset / "new"
    measured = records[-10:]
    times = [sum(row["ns"]) for row in measured]
    mean = statistics.mean(t / row["iterations"] for t, row in zip(times, measured))
    write(path / "sample.json", {"sampling_mode": "Flat",
          "iters": [row["iterations"] for row in measured], "times": times})
    write(path / "estimates.json", {"mean": {"point_estimate": mean,
          "confidence_interval": {"confidence_level": .95,
          "lower_bound": mean * .99, "upper_bound": mean * 1.01}}})
    write(path / "benchmark.json", {"group_id": "paired", "function_id": dataset,
          "value_str": None, "full_id": f"paired/{dataset}",
          "directory_name": f"paired/{dataset}"})


def paired_fixture(root, phase="calibration", ratios=None):
    manifest = fixture(root, phase)
    for index, run in enumerate(manifest["runs"]):
        run_root = root / run["name"]
        shutil.rmtree(run_root / "range")
        inputs = json.loads((run_root / "inputs.json").read_text())
        inputs.update(protocol="interleaved-v2", schedule="splitmix64-fisher-yates-fnv1a64-v1",
            backends=LABELS, order_index=ORDERS.index(run["order"]), batches_per_chunk=16384,
            cases_per_batch=1024, chunks_per_label_cycle=10, calls_per_label_cycle=167772160,
            mode="measurement", criterion={"samples": 10, "warmup_ns": 2000000000,
                "measurement_ns": 5000000000, "confidence_level": .95, "sampling_mode": "Flat"},
            timer_probe={"count": 10000, "p99_ns": 1, "ns": [1] * 10000})
        write(run_root / "inputs.json", inputs)
        factor = 1 if ratios is None else ratios[index]
        for dataset in DATASETS:
            records = [{"iterations": 1, "cycle_start": cycle,
                "ns": [100000, 100000, 97000, 100000, round(100000 * factor)],
                "min_chunk_ns": [1000] * 5,
                "schedule_digest": schedule_digest(inputs["order_index"], cycle, 1)}
                for cycle in range(11)]
            save_case(run_root, dataset, records)
    return manifest


class PairedTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name) / "calibration"
        self.manifest = paired_fixture(self.root)
        self.run = self.root / "run-01"
        self.sidecar = self.run / "paired-overlap.json"
        self.inputs = self.run / "inputs.json"
        self.case = self.run / "paired" / "overlap" / "new"

    def mutate(self, path, mutate):
        value = json.loads(path.read_text())
        mutate(value)
        write(path, value)

    def invalid(self, pattern):
        with self.assertRaisesRegex(assess.InvalidEvidence, pattern):
            paired.assess(self.root, HASH)

    def test_schedule_matches_rust_independent_digest_vectors(self):
        for order, start, count, expected in (
                (0, 0, 1, "158499ad158ee825"), (1, 0, 1, "0727b6e5cf22add9"),
                (2, 0, 1, "c5bce0279f3b92d1"), (1, 7, 3, "fba4c0f9dc773d2d")):
            self.assertEqual(paired.schedule_digest(order, start, count), expected)

    def test_valid_pilot_preserves_absolute_costs_and_all_invocations(self):
        result = paired.assess(self.root, HASH)
        self.assertTrue(result["valid"])
        run = result["run_results"][0]["overlap"]
        self.assertEqual(run["warmup_records"], 1)
        self.assertEqual(run["measured_records"], 10)
        self.assertAlmostEqual(run["ratios"]["rust/reference"], .97)
        self.assertAlmostEqual(run["ns_per_call"]["rust"], 97000 / 167772160)
        self.assertEqual(run["calls_per_label"], 1677721600)

    def test_sequential_protocol_and_smoke_are_rejected(self):
        for key, value in (("protocol", "sequential"), ("protocol", "interleaved-v1"), ("mode", "smoke")):
            with self.subTest(key=key):
                old = json.loads(self.inputs.read_text())
                self.mutate(self.inputs, lambda data: data.update({key: value}))
                self.invalid("protocol|measurement")
                write(self.inputs, old)

    def test_dataset_and_label_identity_rejected(self):
        for key, value in (("backends", LABELS[::-1]), ("order_index", 2),
                           ("calls_per_label_cycle", 1)):
            with self.subTest(key=key):
                old = json.loads(self.inputs.read_text())
                self.mutate(self.inputs, lambda data: data.update({key: value}))
                self.invalid("metadata|order|work")
                write(self.inputs, old)
        self.mutate(self.inputs, lambda data: data["datasets"][0].update(fnv1a64_le="0" * 16))
        self.invalid("fingerprint")

    def test_missing_extra_and_wrong_case_evidence_rejected(self):
        self.mutate(self.case / "benchmark.json", lambda data: data.update(function_id="before"))
        self.invalid("identity")
        self.mutate(self.case / "benchmark.json", lambda data: data.update(function_id="overlap"))
        write(self.run / "paired-extra.json", {})
        self.invalid("case evidence")
        (self.run / "paired-extra.json").unlink()
        (self.case / "sample.json").unlink()
        self.invalid("case evidence|missing")

    def test_extra_case_in_another_group_is_rejected(self):
        write(self.run / "range" / "overlap" / "c" / "new" / "benchmark.json", {})
        self.invalid("case evidence")

    def test_dataset_count_cannot_be_a_float_or_boolean(self):
        for count in (1024.0, True):
            with self.subTest(count=count):
                self.mutate(self.inputs, lambda data: data["datasets"][0].update(cases=count))
                self.invalid("fingerprint")

    def test_sidecar_digest_and_contiguous_warmup_cycles_rejected(self):
        for field, value, error in (("schedule_digest", "0" * 16, "schedule"),
                                    ("cycle_start", 8, "continuity")):
            old = json.loads(self.sidecar.read_text())
            self.mutate(self.sidecar, lambda data: data["records"][0].update({field: value}))
            self.invalid(error)
            write(self.sidecar, old)

    def test_sidecar_integer_durations_minima_and_overflow_rejected(self):
        for field, value in (("iterations", 1.0), ("ns", [100000] * 4),
                             ("ns", [True] * 5), ("ns", [1 << 64] * 5),
                             ("min_chunk_ns", [20000] * 5)):
            with self.subTest(field=field, value=value):
                old = json.loads(self.sidecar.read_text())
                self.mutate(self.sidecar, lambda data: data["records"][0].update({field: value}))
                self.invalid("integer|duration|minimum")
                write(self.sidecar, old)

    def test_corrupt_record_and_cycle_counts_exceeding_capacity_are_rejected(self):
        original = json.loads(self.sidecar.read_text())
        too_many = dict(original, records=original["records"] + [original["records"][-1]] * 1014)
        write(self.sidecar, too_many)
        with self.subTest(kind="records"):
            self.invalid("capacity")
        write(self.sidecar, original)
        original["records"][0].update(iterations=65537, ns=[655370000] * 5)
        write(self.sidecar, original)
        with self.subTest(kind="cycles"):
            self.invalid("capacity")

    def test_flat_samples_must_exactly_match_last_ten_records(self):
        for field, value in (("iters", [2] * 10), ("times", [497001] * 10),
                             ("sampling_mode", "Linear")):
            old = json.loads((self.case / "sample.json").read_text())
            self.mutate(self.case / "sample.json", lambda data: data.update({field: value}))
            self.invalid("sample|Flat")
            write(self.case / "sample.json", old)
        self.mutate(self.sidecar, lambda data: data["records"].append(dict(data["records"][-1])))
        self.invalid("continuity")

    def test_mean_and_ci_must_be_positive_finite_current_and_95_percent(self):
        for mutate in (lambda data: data["mean"].update(point_estimate=3),
                       lambda data: data["mean"]["confidence_interval"].update(confidence_level=.9),
                       lambda data: data["mean"]["confidence_interval"].update(upper_bound=float("inf"))):
            old = json.loads((self.case / "estimates.json").read_text())
            self.mutate(self.case / "estimates.json", mutate)
            self.invalid("mean|CI|finite")
            write(self.case / "estimates.json", old)

    def test_probe_raw_distribution_p99_and_budget_are_checked(self):
        for mutate in (lambda data: data["timer_probe"].update(count=9999),
                       lambda data: data["timer_probe"].update(p99_ns=2),
                       lambda data: data["timer_probe"].update(ns=[6] * 10000, p99_ns=6)):
            old = json.loads(self.inputs.read_text())
            self.mutate(self.inputs, mutate)
            self.invalid("probe|timestamp")
            write(self.inputs, old)

    def test_paired_ratio_drift_is_checked_for_all_four_pairs(self):
        for label in (0, 1, 2, 4):
            old = json.loads(self.sidecar.read_text())
            for row in old["records"][-2:]:
                row["ns"][label] *= 2
            save_case(self.run, "overlap", old["records"])
            self.invalid("drift")
            paired_fixture(self.root)

    def test_pilot_aa_bias_is_a_gate_but_absolute_baseline_spread_is_diagnostic(self):
        data = json.loads(self.sidecar.read_text())
        for row in data["records"]:
            row["ns"][4] = 103000
        save_case(self.run, "overlap", data["records"])
        self.invalid("A/A.*pilot")
        paired_fixture(self.root)
        data = json.loads(self.sidecar.read_text())
        for row in data["records"]:
            row["ns"][3] *= 4
            row["ns"][4] *= 4
        save_case(self.run, "overlap", data["records"])
        report = paired.assess(self.root, HASH)
        self.assertEqual(report["baseline_spread"]["overlap"]["baseline-a"], 4)

    def test_run_ratio_uses_total_duration_instead_of_mean_sample_ratios(self):
        data = json.loads(self.sidecar.read_text())
        for index, row in enumerate(data["records"]):
            row["ns"][1] = 100000 if index % 2 else 200000
            row["ns"][2] = 100000 if index % 2 else 300000
        save_case(self.run, "overlap", data["records"])
        report = paired.assess(self.root, HASH)
        self.assertAlmostEqual(report["run_results"][0]["overlap"]["ratios"]["rust/reference"], 4 / 3)

    def test_missing_run_guest_affinity_and_host_trace_still_rejected(self):
        for mutate in (lambda data: data["runs"].pop(),
                       lambda data: data["runs"][0].update(guest_affinity=[])):
            old = json.loads((self.root / "manifest.json").read_text())
            self.mutate(self.root / "manifest.json", mutate)
            self.invalid("number|affinity")
            write(self.root / "manifest.json", old)
        (self.root / "host.jsonl").unlink()
        self.invalid("host trace")

    def test_six_block_estimator_and_aa_interval_not_each_comparison_point(self):
        comparison = self.root.parent / "comparison"
        # Each block is exactly balanced despite individual ±3% process effects.
        paired_fixture(comparison, "comparison", [1.03, 1 / 1.03, 1] * 6)
        report = paired.assess(comparison, HASH, self.root)
        self.assertEqual(len(report["pairs"]), 16)
        aa = report["pairs"]["baseline-a/baseline-b:overlap"]
        self.assertAlmostEqual(aa["geometric_ratio"], 1, places=5)
        self.assertEqual(aa["classification"], "equivalent")
        values = [{dataset: {"ratios": {"baseline-a/baseline-b": math.exp(v)}}
                   for dataset in DATASETS} for v in [x for b in (-.01, 0, .01, .02, -.02, .03) for x in [b] * 3]]
        actual = paired.block_report(values, (("baseline-a", "baseline-b"),))
        interval = actual["baseline-a/baseline-b:overlap"]["ci95"]
        self.assertAlmostEqual(interval[0], .985473401043389)
        self.assertAlmostEqual(interval[1], 1.0249390455539014)

    def test_comparison_invalidated_when_final_aa_interval_exits_equivalence_band(self):
        comparison = self.root.parent / "comparison"
        paired_fixture(comparison, "comparison", [1.04] * 18)
        with self.assertRaisesRegex(assess.InvalidEvidence, "A/A.*CI"):
            paired.assess(comparison, HASH, self.root)

    def test_comparison_still_checks_timer_budget_ratio_drift_and_raw_samples(self):
        comparison = self.root.parent / "comparison"
        paired_fixture(comparison, "comparison")
        run = comparison / "run-18"
        inputs = run / "inputs.json"
        original = json.loads(inputs.read_text())
        self.mutate(inputs, lambda data: data["timer_probe"].update(ns=[6] * 10000, p99_ns=6))
        with self.assertRaisesRegex(assess.InvalidEvidence, "timestamp"):
            paired.assess(comparison, HASH, self.root)
        write(inputs, original)
        sidecar = run / "paired-overlap.json"
        records = json.loads(sidecar.read_text())["records"]
        for row in records[-2:]:
            row["ns"][2] *= 2
        save_case(run, "overlap", records)
        with self.assertRaisesRegex(assess.InvalidEvidence, "drift"):
            paired.assess(comparison, HASH, self.root)
        self.mutate(run / "paired" / "overlap" / "new" / "sample.json",
                    lambda data: data["times"].__setitem__(0, 1))
        with self.assertRaisesRegex(assess.InvalidEvidence, "sample time"):
            paired.assess(comparison, HASH, self.root)


if __name__ == "__main__":
    unittest.main()
