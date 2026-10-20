import json
import tempfile
import unittest
from pathlib import Path

from summarize import summarize_order


class SummaryTest(unittest.TestCase):
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
