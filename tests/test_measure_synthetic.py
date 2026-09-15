#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure synthetic detector tests; no ffmpeg, media files or hardware required."""

import importlib.util
import contextlib
import io
from pathlib import Path
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "measure_synthetic.py"
SPEC = importlib.util.spec_from_file_location("measure_synthetic", MODULE_PATH)
measure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measure)


class SyntheticMeasurementTests(unittest.TestCase):
    def test_known_offsets_preserve_absolute_time(self):
        video = [time + 0.7 for time in measure.EVENT_SECONDS]
        desktop = [time + 0.125 for time in video]
        microphone = [time - 0.042 for time in video]
        result = measure.compare_onsets(video, [desktop, microphone])
        self.assertTrue(result["valid_marker_match"])
        self.assertAlmostEqual(result["tracks"][0]["median_offset_ms"], 125)
        self.assertAlmostEqual(result["tracks"][1]["median_offset_ms"], -42)
        self.assertAlmostEqual(result["tracks"][0]["peak_to_peak_jitter_ms"], 0)

    def test_injected_rms_and_flash_signals(self):
        # Simulate a decoded negative audio start and a known35ms delay.
        audio_times = [-0.021 + n / 1000 for n in range(17_000)]
        video_times = [n / 1000 for n in range(17_000)]
        audio_levels = [0.1 if any(0 <= time - (event + 0.035) < 0.04
                                  for event in measure.EVENT_SECONDS) else 0 for time in audio_times]
        video_levels = [200 if any(0 <= time - event < 0.1
                                  for event in measure.EVENT_SECONDS) else 0 for time in video_times]
        audio = measure.threshold_onsets(audio_times, audio_levels, [0.001] * len(audio_times), 0.01)
        video = measure.threshold_onsets(video_times, video_levels, [0.001] * len(video_times), 100)
        report = measure.compare_onsets(video, [audio, audio])
        self.assertTrue(report["valid_marker_match"])
        self.assertAlmostEqual(report["tracks"][0]["median_offset_ms"], 35, delta=1.01)

    def test_missing_event_never_subset_matches(self):
        video = list(measure.EVENT_SECONDS)
        audio = video[:3] + video[4:]
        report = measure.compare_onsets(video, [audio, video])
        self.assertFalse(report["valid_marker_match"])
        self.assertEqual(report["tracks"][0]["fingerprint"]["missing_count"], 1)
        self.assertNotIn("audio_minus_video_ms", report["tracks"][0])

    def test_extra_event_is_rejected(self):
        video = list(measure.EVENT_SECONDS)
        audio = sorted(video + [3.0])
        report = measure.compare_onsets(video, [audio, video])
        self.assertFalse(report["valid_marker_match"])
        self.assertEqual(report["tracks"][0]["fingerprint"]["extra_or_duplicate_count"], 1)

    def test_equal_count_but_mutated_interval_is_rejected(self):
        video = list(measure.EVENT_SECONDS)
        audio = video.copy()
        audio[3] += 0.2
        report = measure.compare_onsets(video, [audio, video])
        self.assertFalse(report["valid_marker_match"])
        self.assertNotIn("median_offset_ms", report["tracks"][0])

    def test_no_modulo_or_cycle_shift(self):
        video = list(measure.EVENT_SECONDS)
        later = [time + 20 for time in video]
        report = measure.compare_onsets(video, [later, later])
        self.assertTrue(report["valid_marker_match"])
        self.assertAlmostEqual(report["tracks"][0]["median_offset_ms"], 20_000)

    def test_jitter_is_measured(self):
        video = list(measure.EVENT_SECONDS)
        audio = [time + offset for time, offset in zip(video, [0.020, 0.021, 0.024, 0.019, 0.022, 0.020])]
        result = measure.compare_onsets(video, [audio, audio])
        self.assertTrue(result["valid_marker_match"])
        self.assertAlmostEqual(result["tracks"][0]["peak_to_peak_jitter_ms"], 5)

    def test_ambiguous_click_is_not_hidden(self):
        with self.assertRaises(measure.MeasurementError):
            measure.threshold_onsets([0, 0.001, 0.002], [0, 1, 0], [0.001] * 3, 0.5)

    def test_bad_track_count_and_nonfinite(self):
        with self.assertRaises(measure.MeasurementError):
            measure.compare_onsets(measure.EVENT_SECONDS, [measure.EVENT_SECONDS])
        with self.assertRaises(measure.MeasurementError):
            measure.fingerprint([float("nan")] * 6)
        with self.assertRaises(measure.MeasurementError):
            measure.decoded_times([{"pts_time": "0"}, {"pts_time": "0"}])

    def test_multiple_complete_cycles_and_cross_cycle_gap(self):
        schedule = measure.expected_schedule(3)
        self.assertEqual(len(schedule), 18)
        self.assertAlmostEqual(schedule[6] - schedule[5], 6.2)
        video = [time - 0.4 for time in schedule]
        audio = [time + 0.006 for time in video]
        report = measure.compare_onsets(video, [audio, audio], cycles=3,
                                        max_median_ms=16.667, max_offset_ms=33.333)
        self.assertTrue(report["valid_marker_match"])
        self.assertTrue(report["timing_gate"]["passed"])
        self.assertEqual(report["tracks"][0]["fingerprint"]["expected_count"], 18)
        for offset in report["tracks"][0]["cycle_median_offset_ms"]:
            self.assertAlmostEqual(offset, 6)

    def test_multicycle_wrong_count_and_boundary_shift_rejected(self):
        schedule = list(measure.expected_schedule(2))
        missing = schedule[:8] + schedule[9:]
        report = measure.compare_onsets(schedule, [missing, schedule], cycles=2, max_offset_ms=33.333)
        self.assertFalse(report["valid_marker_match"])
        self.assertIsNone(report["timing_gate"]["passed"])
        changed_boundary = [time + (0.2 if n >= 6 else 0) for n, time in enumerate(schedule)]
        self.assertFalse(measure.fingerprint(changed_boundary, cycles=2)["valid"])

    def test_default_still_requires_exactly_six_events(self):
        report = measure.compare_onsets(measure.expected_schedule(2), [measure.expected_schedule(2)] * 2)
        self.assertFalse(report["valid_marker_match"])
        self.assertEqual(report["video_fingerprint"]["expected_count"], 6)

    def test_median_failure_is_distinct_from_marker_failure(self):
        video = list(measure.EVENT_SECONDS)
        audio = [time - 0.020 for time in video]
        report = measure.compare_onsets(video, [audio, audio], max_median_ms=16.667, max_offset_ms=33.333)
        self.assertTrue(report["valid_marker_match"])
        self.assertFalse(report["timing_gate"]["passed"])
        self.assertIn("median", report["tracks"][0]["timing_gate"]["violations"][0])

    def test_one_outlier_fails_max_offset_even_when_median_passes(self):
        video = list(measure.EVENT_SECONDS)
        audio = [time + (0.04 if n == 3 else 0.005) for n, time in enumerate(video)]
        report = measure.compare_onsets(video, [audio, video], max_median_ms=16.667, max_offset_ms=33.333)
        self.assertTrue(report["valid_marker_match"])
        self.assertAlmostEqual(report["tracks"][0]["median_offset_ms"], 5)
        self.assertFalse(report["timing_gate"]["passed"])
        self.assertTrue(report["tracks"][1]["timing_gate"]["passed"])

    def test_gate_not_requested_is_not_reported_as_passed(self):
        report = measure.compare_onsets(measure.EVENT_SECONDS, [measure.EVENT_SECONDS] * 2)
        self.assertTrue(report["valid_marker_match"])
        self.assertFalse(report["timing_gate"]["requested"])
        self.assertIsNone(report["timing_gate"]["passed"])

    def test_split_tone_regions_survive_threshold_changes(self):
        times = [n / 1000 for n in range(70)]
        levels = [0.2 if 0 <= time < 0.010 or 0.030 <= time < 0.060 else 0 for time in times]
        for threshold in (0.005, 0.01, 0.02, 0.04):
            regions = measure.threshold_regions(times, levels, [0.001] * len(times), threshold)
            self.assertEqual(len(regions), 2)
            self.assertAlmostEqual(regions[1]["start_seconds"] - regions[0]["end_seconds"], 0.020)
            onsets = measure.region_onsets(regions, min_duration=0.010, max_duration=0.080)
            self.assertEqual(onsets, [0, 0.030])

    def test_invalid_cycles_and_limits(self):
        for cycles in (0, -1, 7, 1.5, True):
            with self.assertRaises(measure.MeasurementError):
                measure.expected_schedule(cycles)
        for limit in (-1, float("nan"), float("inf")):
            with self.assertRaises(measure.MeasurementError):
                measure.compare_onsets(measure.EVENT_SECONDS, [measure.EVENT_SECONDS] * 2,
                                        max_median_ms=limit)

    def test_cli_timing_failure_exit_code(self):
        metadata = {"format": {"duration": "18"}, "streams": [
            {"index": 0, "codec_type": "video"}, {"index": 1, "codec_type": "audio"},
            {"index": 2, "codec_type": "audio"}]}
        video = list(measure.EVENT_SECONDS)
        delayed = [time + 0.020 for time in video]
        with mock.patch.object(measure, "probe", return_value=metadata), \
                mock.patch.object(measure, "measure_video", return_value=(video, {})), \
                mock.patch.object(measure, "measure_audio", return_value=(delayed, {})), \
                contextlib.redirect_stdout(io.StringIO()) as output:
            status = measure.main(["synthetic.mkv", "--max-median-ms", "16.667", "--max-offset-ms", "33.333"])
        self.assertEqual(status, 3)
        report = measure.json.loads(output.getvalue())
        self.assertTrue(report["valid_marker_match"])
        self.assertFalse(report["timing_gate"]["passed"])


if __name__ == "__main__":
    unittest.main()
