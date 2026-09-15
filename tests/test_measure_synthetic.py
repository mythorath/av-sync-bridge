#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure synthetic detector tests; no ffmpeg, media files or hardware required."""

import importlib.util
from pathlib import Path
import unittest

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


if __name__ == "__main__":
    unittest.main()
