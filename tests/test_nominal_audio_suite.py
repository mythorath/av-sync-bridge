# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location("run_nominal_audio_suite", Path(__file__).parents[1] / "tools/run_nominal_audio_suite.py")
suite = importlib.util.module_from_spec(spec)
spec.loader.exec_module(suite)


class NominalSuiteTests(unittest.TestCase):
    def report(self, case):
        target = case["seconds"] * 48000
        nominal_ns = case["seconds"] * 10**9
        source_ns = case["seconds"] * 10**15 // (1_000_000 + case["ppm"])
        return dict(schema=1, generated_only=True, timestamps_passed_to_converter=False, capture_metadata_only=True,
                    live_sync_proven=False, passed=not case["negative"], signal=case["signal"],
                    seconds=case["seconds"], source_rate=case["rate"],
                    source_channels=8 if case["rate"] == 192000 else 2,
                    frequency_hz=case["frequency"], source_ppm=case["ppm"], negative_control=case["negative"],
                    input_frames=case["seconds"] * case["rate"], fixed_output_frames=target,
                    varied_output_frames=target, fixed_before_finish_frames=target-100,
                    varied_before_finish_frames=target-100, max_latency_input_frames=400,
                    original_capture_duration_ns=source_ns, nominal_output_duration_ns=nominal_ns,
                    capture_minus_nominal_ns=source_ns-nominal_ns,
                    partition_max_error=0.1 if case["negative"] else 0,
                    residual_rms_dbfs=-120, maximum_residual_dbfs=-100,
                    fixed_gain_db=0, varied_gain_db=0, right_peak_dbfs=-300,
                    output_peak=0 if case["signal"] == "silence" else 0.5,
                    marker_regions=6, max_marker_error_samples=0,
                    max_impulse_error_samples=0, impulse_peak=0.1)

    def test_bounded_matrix(self):
        self.assertEqual(len(suite.cases()), 56)
        self.assertEqual(len(suite.cases(long=True)), 59)
        self.assertEqual(len(suite.cases(quick=True)), 5)
        self.assertEqual({case["rate"] for case in suite.cases()}, {44100, 48000, 192000})
        self.assertTrue(all(2 <= case["seconds"] <= 90 for case in suite.cases(long=True)))

    def test_independent_metadata_clock(self):
        for case in suite.cases():
            report = self.report(case)
            code = 3 if case["negative"] else 0
            self.assertTrue(suite.valid_result(case, code, report))
            report["original_capture_duration_ns"] += 1
            self.assertFalse(suite.valid_result(case, code, report))

    def test_false_success_rejected(self):
        case = suite.cases()[0]
        for key, value in (("partition_max_error", 0.001), ("fixed_output_frames", 5),
                           ("varied_before_finish_frames", 90000), ("residual_rms_dbfs", -30),
                           ("maximum_residual_dbfs", -20), ("right_peak_dbfs", -60),
                           ("output_peak", 1.1), ("fixed_gain_db", float("nan")),
                           ("input_frames", True), ("timestamps_passed_to_converter", True),
                           ("live_sync_proven", True)):
            report = self.report(case)
            report[key] = value
            self.assertFalse(suite.valid_result(case, 0, report), key)
        self.assertFalse(suite.valid_result(case, 0, []))

    def test_negative_requires_measured_difference(self):
        case = [case for case in suite.cases() if case["negative"]][0]
        report = self.report(case)
        self.assertTrue(suite.valid_result(case, 3, report))
        self.assertFalse(suite.valid_result(case, 0, report))
        report["partition_max_error"] = 0
        self.assertFalse(suite.valid_result(case, 3, report))

    def test_signal_specific_gate(self):
        for signal, key, value in (("markers", "marker_regions", 5),
                                   ("impulse", "max_impulse_error_samples", 8),
                                   ("silence", "output_peak", 0.1)):
            case = next(case for case in suite.cases() if case["signal"] == signal)
            report = self.report(case)
            report[key] = value
            self.assertFalse(suite.valid_result(case, 0, report))


if __name__ == "__main__":
    unittest.main()
