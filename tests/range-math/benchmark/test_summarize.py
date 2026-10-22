import json
import tempfile
import unittest
from pathlib import Path

from summarize import summarize_order


class SummaryTest(unittest.TestCase):
    def test_reference_mode_reports_fresh_point_ratios_and_actual_intervals(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "inputs.json").write_text(json.dumps({
                "seed": "fixed", "order": "c-first",
                "backends": ["c", "reference", "rust", "baseline"],
                "datasets": [{"name": "overlap", "cases": 1024, "fnv1a64_le": "fixed"}],
            }))
            for backend, value in [("c", 10240.0), ("reference", 20480.0),
                                   ("rust", 15360.0), ("baseline", 5120.0)]:
                path = root / "range" / "overlap" / backend / "new"
                path.mkdir(parents=True)
                (path / "estimates.json").write_text(json.dumps({
                    "mean": {"point_estimate": value, "confidence_interval": {
                        "confidence_level": 0.95,
                        "lower_bound": value * 0.9, "upper_bound": value * 1.1}},
                }))
            result = summarize_order(root, ["overlap"], reference=True)["overlap"]
            self.assertEqual(result["reference"]["ns_per_call"], 20.0)
            self.assertEqual(result["reference"]["confidence_level"], 0.95)
            self.assertEqual(result["rust_over_reference_point"], 0.75)
            self.assertEqual(result["reference_over_c_point"], 2.0)
            self.assertEqual(result["rust_over_c_point"], 1.5)

    def test_reference_order_metadata_rejects_mismatched_root(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            (root / "inputs.json").write_text(json.dumps({
                "seed": "fixed", "order": "rust-first",
                "backends": ["rust", "c", "reference", "baseline"],
                "datasets": [{"name": "overlap", "cases": 1024, "fnv1a64_le": "fixed"}],
            }))
            with self.assertRaisesRegex(ValueError, "order"):
                summarize_order(root, ["overlap"], reference=True, expected_order="c-first")

    def test_normalizes_batch_nanoseconds_and_keeps_baseline_raw(self):
        with tempfile.TemporaryDirectory() as temp:
            root = Path(temp)
            for backend, value in [("c", 10240.0), ("rust", 15360.0), ("baseline", 5120.0)]:
                path = root / "range" / "overlap" / backend / "new"
                path.mkdir(parents=True)
                (path / "estimates.json").write_text(json.dumps({
                    "slope": {"point_estimate": value, "confidence_interval": {
                        "confidence_level": 0.90,
                        "lower_bound": value * 0.9, "upper_bound": value * 1.1}},
                    "mean": {"point_estimate": value * 2, "confidence_interval": {
                        "confidence_level": 0.90,
                        "lower_bound": value * 1.8, "upper_bound": value * 2.2}},
                }))
            result = summarize_order(root, ["overlap"])["overlap"]
            self.assertEqual(result["c"], {
                "estimator": "slope", "ns_per_call": 10.0,
                "confidence_level": 0.90,
                "ci_ns_per_call": [9.0, 11.0],
            })
            self.assertEqual(result["rust"]["ns_per_call"], 15.0)
            self.assertEqual(result["baseline"]["ns_per_call"], 5.0)
            self.assertEqual(result["rust_over_c_point"], 1.5)
            self.assertNotIn("ratio_ci95", result)


if __name__ == "__main__":
    unittest.main()
