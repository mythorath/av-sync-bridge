#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
from pathlib import Path
import sys
import unittest

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
try:
    SPEC = importlib.util.spec_from_file_location("measure_mixer_trace", TOOLS / "measure_mixer_trace.py")
    measure = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(measure)
finally:
    sys.path.pop(0)


class MixerTraceTests(unittest.TestCase):
    header = {"epoch_ns": 1_000_000_000_000, "delay_ms": 2000, "fps": 60}

    def rows(self, offset_ns=0, cycles=1, missing=None):
        starts = [round(time * 1e9) + offset_ns for time in measure.intended_offsets(self.header, cycles)]
        if missing is not None:
            del starts[missing]
        for start in starts:
            for delta in range(-5_000_000, 45_000_000, 1_000_000):
                yield {"timestamp_ns": self.header["epoch_ns"] + start + delta,
                       "frames": 48, "rms": 0.1 if 0 <= delta < 40_000_000 else 0}

    def test_known_epoch_offset_and_explicit_gate(self):
        result = measure.analyze_rows(self.rows(21_000_000), self.header, max_offset_ms=1)
        self.assertTrue(result["valid_marker_match"])
        self.assertFalse(result["timing_gate"]["passed"])
        self.assertAlmostEqual(result["median_offset_ms"], 21)
        self.assertNotIn("epoch_ns", result)

    def test_zero_and_negative_window_quantization(self):
        for offset in (0, -500_000):
            report = measure.analyze_rows(self.rows(offset), self.header, max_offset_ms=1)
            self.assertTrue(report["timing_gate"]["passed"])
            self.assertAlmostEqual(report["median_offset_ms"], offset / 1e6)

    def test_complete_multicycle_and_missing_rejection(self):
        report = measure.analyze_rows(self.rows(cycles=2), self.header, cycles=2)
        self.assertTrue(report["valid_marker_match"])
        self.assertEqual(report["expected_count"], 12)
        report = measure.analyze_rows(self.rows(missing=2), self.header, max_offset_ms=1)
        self.assertFalse(report["valid_marker_match"])
        self.assertIsNone(report["timing_gate"]["passed"])
        self.assertNotIn("raw_minus_intended_ms", report)

    def test_producer_header_one_session_only(self):
        text = "SYNTHETIC generation=7 epoch_ns=1000000000000 size=640x360 fps=60 delay_ms=2000 path=unused\n"
        self.assertEqual(measure.parse_producer_log(text), self.header)
        for bad in ("", text + text, text.replace("fps=60", "fps=0")):
            with self.assertRaises(measure.MeasurementError):
                measure.parse_producer_log(bad)

    def test_producer_frame_rounding_is_preserved(self):
        header = dict(self.header, fps=24)
        expected = measure.intended_offsets(header, 1)
        self.assertAlmostEqual(expected[1], 2 + 56 / 24)

    def test_invalid_trace_rows_and_overflow(self):
        for row in ({"timestamp_ns": 1, "frames": 49, "rms": 0.1},
                    {"timestamp_ns": 1, "frames": 48, "rms": float("nan")},
                    {"timestamp_ns": -1, "frames": 48, "rms": 0.1}):
            with self.assertRaises(measure.MeasurementError):
                measure.analyze_rows([row], self.header)
        row = {"timestamp_ns": 1, "frames": 48, "rms": 0.1}
        with self.assertRaises(measure.MeasurementError):
            measure.analyze_rows([row, row], self.header)
        with self.assertRaises(measure.MeasurementError):
            measure.intended_offsets(dict(self.header, epoch_ns=measure.MAX_SIGNED_NS), 1)

    def test_explicit_generation_window_reports_prior_regions(self):
        prior = [{"timestamp_ns": self.header["epoch_ns"] + 1_000_000_000 + n * 1_000_000,
                  "frames": 48, "rms": 0.1} for n in range(40)]
        rows = prior + list(self.rows())
        strict = measure.analyze_rows(rows, self.header)
        self.assertFalse(strict["valid_marker_match"])
        scoped = measure.analyze_rows(rows, self.header, current_generation_window=True, max_offset_ms=2)
        self.assertTrue(scoped["valid_marker_match"])
        self.assertTrue(scoped["timing_gate"]["passed"])
        self.assertEqual(scoped["selection"]["ignored_prior_regions"], 1)
        self.assertEqual(scoped["selection"]["start_relative_epoch_seconds"], 2)
        self.assertEqual(scoped["selection"]["end_relative_epoch_seconds"], 18)

    def test_generation_window_does_not_hide_inside_extra_or_straddling_tone(self):
        inside = [{"timestamp_ns": self.header["epoch_ns"] + 2_500_000_000 + n * 1_000_000,
                   "frames": 48, "rms": 0.1} for n in range(40)]
        report = measure.analyze_rows(inside + list(self.rows()), self.header, current_generation_window=True)
        self.assertFalse(report["valid_marker_match"])
        self.assertEqual(report["extra_or_duplicate_count"], 1)
        crossing = [{"timestamp_ns": self.header["epoch_ns"] + 1_990_000_000 + n * 1_000_000,
                     "frames": 48, "rms": 0.1} for n in range(40)]
        with self.assertRaises(measure.MeasurementError):
            measure.analyze_rows(crossing + list(self.rows()), self.header, current_generation_window=True)


if __name__ == "__main__":
    unittest.main()
