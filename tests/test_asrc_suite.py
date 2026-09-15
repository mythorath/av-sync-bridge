# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("run_asrc_suite", Path(__file__).parents[1] / "tools/run_asrc_suite.py")
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)


class SuiteTests(unittest.TestCase):
    def report(self, case):
        frames = 384000 + (192 if case["ppm"] == 500 else -192 if case["ppm"] == -500 else 0)
        output = 384000 if case["expected_pass"] else frames
        error = output - frames * 1_000_000 // (1_000_000 + case["ppm"])
        return dict(schema=1, generated_only=True, live_controller=False, passed=case["expected_pass"],
                    source_ppm=case["ppm"], signal=case["signal"], seconds=case["seconds"],
                    frequency_hz=case["frequency"], chunk_pattern="varied" if case["varied"] else "fixed",
                    original_anchor_preflight=case["estimate"], input_frames=frames, output_frames=output,
                    source_duration_error_frames=error, count_error_frames=0, residual_rms_dbfs=-120,
                    maximum_residual_dbfs=-100, right_peak_dbfs=-300, gain_db=0,
                    max_marker_error_samples=0, output_peak=0.5, marker_regions=6)
    def test_bounded_matrix(self):
        self.assertEqual(len(suite.cases()), 78)
        self.assertEqual(len(suite.cases(long=True)), 79)
        self.assertEqual(len(suite.cases(quick=True)), 5)
        self.assertTrue(all(4 <= case["seconds"] <= 600 for case in suite.cases(long=True)))

    def test_stationary_negative_controls(self):
        controls = [case for case in suite.cases() if not case["expected_pass"]]
        self.assertEqual(len(controls), 8)
        self.assertEqual({case["signal"] for case in controls}, {"tone", "markers", "silence", "dc"})
        case = controls[-1]
        report = self.report(case)
        self.assertTrue(suite.valid_result(case, 3, report))
        self.assertFalse(suite.valid_result(case, 2, report))
        report["source_duration_error_frames"] = 0
        self.assertFalse(suite.valid_result(case, 3, report))
        report["passed"] = True
        self.assertFalse(suite.valid_result(case, 0, report))

    def test_positive_requires_full_report(self):
        case = suite.cases()[0]
        report = self.report(case)
        self.assertTrue(suite.valid_result(case, 0, report))
        self.assertFalse(suite.valid_result(case, 3, report))
        report.pop("source_duration_error_frames")
        self.assertFalse(suite.valid_result(case, 0, report))

    def test_independent_gates_reject_false_success(self):
        case = suite.cases()[0]
        for key, value in (("residual_rms_dbfs", -20), ("output_peak", 1.1), ("gain_db", float("nan")),
                           ("count_error_frames", 30), ("output_frames", 400000), ("source_duration_error_frames", True)):
            report = self.report(case)
            report[key] = value
            self.assertFalse(suite.valid_result(case, 0, report), key)
        self.assertFalse(suite.valid_result(case, 0, []))


if __name__ == "__main__":
    unittest.main()
