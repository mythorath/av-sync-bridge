#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Detector validation tests, not physical hardware synchronization evidence."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import sys
import unittest
from unittest import mock

try:
    import numpy as np
except ImportError:
    np = None

SPEC = importlib.util.spec_from_file_location(
    "measure_physical", Path(__file__).resolve().parents[1] / "tools" / "measure_physical.py")
measure = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(measure)


class PhysicalMeasurementTests(unittest.TestCase):
    def test_known_offset_and_negative_timeline_preserved(self):
        video = [value - 6 for value in measure.EVENT_SECONDS]
        audio = [value - 0.123 for value in video]
        result = measure.compare_onsets(video, audio)
        self.assertTrue(result["valid_marker_match"])
        self.assertAlmostEqual(result["median_audio_minus_video_ms"], -123)
        self.assertIsNone(result["timing_gate"]["passed"])

    def test_no_modulo_or_cycle_shift(self):
        result = measure.compare_onsets(measure.EVENT_SECONDS, [x + 68 for x in measure.EVENT_SECONDS])
        self.assertTrue(result["valid_marker_match"])
        self.assertAlmostEqual(result["median_audio_minus_video_ms"], 68_000)

    def test_both_audio_and_video_intervals_are_required(self):
        changed = list(measure.EVENT_SECONDS)
        changed[2] += 0.2
        for video, audio in ((changed, measure.EVENT_SECONDS), (measure.EVENT_SECONDS, changed), (changed, changed)):
            result = measure.compare_onsets(video, audio, max_offset_ms=50)
            self.assertFalse(result["valid_marker_match"])
            self.assertEqual(result["events"], [])
            self.assertIsNone(result["timing_gate"]["passed"])

    def test_missing_extra_duplicate_and_swapped_events_fail(self):
        normal = list(measure.EVENT_SECONDS)
        for wrong in (normal[:-1], normal + [70], normal[:2] + [normal[1]] + normal[3:],
                      normal[:2] + [normal[3], normal[2]] + normal[4:]):
            for video, audio in ((wrong, normal), (normal, wrong)):
                self.assertFalse(measure.compare_onsets(video, audio)["valid_marker_match"])

    def test_nonfinite_and_bad_limits(self):
        for value in (float("nan"), float("inf"), -float("inf")):
            with self.assertRaises(measure.MeasurementError):
                measure.compare_onsets([value] * 6, measure.EVENT_SECONDS)
        for value in (0, -1, float("nan"), 0.151):
            with self.assertRaises(measure.MeasurementError):
                measure.fingerprint(measure.EVENT_SECONDS, value)
        for value in (-1, float("nan"), float("inf")):
            with self.assertRaises(measure.MeasurementError):
                measure.compare_onsets([], [], max_offset_ms=value)

    def test_timing_gate_separate_from_matching(self):
        for delay, passed in ((0.02, True), (0.04, False)):
            result = measure.compare_onsets(measure.EVENT_SECONDS, [x + delay for x in measure.EVENT_SECONDS],
                                            max_median_ms=30, max_offset_ms=35)
            self.assertTrue(result["valid_marker_match"])
            self.assertEqual(result["timing_gate"]["passed"], passed)

    def test_one_outlier_cannot_hide_behind_median(self):
        audio = [x + (0.045 if n == 3 else 0.010) for n, x in enumerate(measure.EVENT_SECONDS)]
        result = measure.compare_onsets(measure.EVENT_SECONDS, audio, max_median_ms=20, max_offset_ms=30)
        self.assertTrue(result["valid_marker_match"])
        self.assertFalse(result["timing_gate"]["passed"])

    def test_timestamps_order_finite_count_and_span(self):
        for times in ((0, 0), (1, 0), (0, float("nan")), (0, 122), (1000, 1001)):
            with self.assertRaises(measure.MeasurementError):
                measure.decoded_times([{"pts_time": x} for x in times], 100)
        with self.assertRaises(measure.MeasurementError):
            measure.decoded_times([{"pts_time": 0}], 100)
        self.assertEqual(measure.decoded_times([{"pts_time": -0.02}, {"pts_time": 0}], 100), [-0.02, 0])

    def test_audio_pts_mapping_retains_negative_start(self):
        frames = [{"pts_time": -0.021, "nb_samples": 1024}, {"pts_time": 0, "nb_samples": 1024}]
        times, counts, starts, residual = measure.audio_layout(frames, 48000)
        self.assertEqual(times, [-0.021, 0])
        self.assertEqual(starts, [0, 1024])
        self.assertEqual(counts, [1024, 1024])
        self.assertLess(residual, 0.001)

    def test_audio_gaps_and_invalid_counts_fail(self):
        for second, count in ((0.1, 1024), (0.021, 0), (0.021, 1.5), (0.021, 48001)):
            with self.assertRaises(measure.MeasurementError):
                measure.audio_layout([{"pts_time": 0, "nb_samples": count},
                                      {"pts_time": second, "nb_samples": count}], 48000)

    def test_regions_include_final_region_and_reject_short_ambiguity(self):
        found = measure.regions([0, 0.1, 0.2, 0.3], [False, True, False, True], 0.1)
        self.assertEqual(len(found), 2)
        self.assertAlmostEqual(found[-1]["end_seconds"], 0.4)
        with self.assertRaises(measure.MeasurementError):
            measure.region_onsets(found, 0.12, 0.3)

    @staticmethod
    def metadata():
        return {"format": {"duration": "70"}, "streams": [
            {"index": 0, "codec_type": "video", "width": 1920, "height": 1080},
            {"index": 1, "codec_type": "audio", "sample_rate": "48000", "channels": 2}]}

    def test_metadata_bounds_and_track_choice(self):
        metadata = self.metadata()
        self.assertEqual(measure.validate_metadata(metadata, 0, 120)[1]["index"], 1)
        for key, value in (("width", 7680), ("height", 4320), ("width", 0)):
            bad = self.metadata()
            bad["streams"][0][key] = value
            with self.assertRaises(measure.MeasurementError):
                measure.validate_metadata(bad, 0, 120)
        for track in (-1, 1, True):
            with self.assertRaises(measure.MeasurementError):
                measure.validate_metadata(metadata, track, 120)
        for duration in ("nan", "121", "0"):
            bad = self.metadata()
            bad["format"]["duration"] = duration
            with self.assertRaises(measure.MeasurementError):
                measure.validate_metadata(bad, 0, 120)

    def test_probe_malformed_json_and_output_limit(self):
        with mock.patch.object(measure, "run", return_value=b"not json"):
            with self.assertRaises(measure.MeasurementError):
                measure.probe("recording.mkv", "ffprobe")
        with mock.patch.object(measure, "run", return_value=b"{}") as run:
            measure.probe("recording.mkv", "ffprobe", 1)
            self.assertEqual(run.call_args.args[1], 8_000_000)
            self.assertIn("%+121", run.call_args.args[0])

    def test_bounded_runner_success_failure_limit_and_timeout(self):
        self.assertEqual(measure.run([sys.executable, "-c", "print('ok')"], 100).strip(), b"ok")
        for script, size, timeout in (("raise SystemExit(2)", 100, 5),
                                      ("import sys; print('decode error', file=sys.stderr)", 100, 5),
                                      ("print('x' * 100000)", 100, 5),
                                      ("import time; time.sleep(5)", 100, 0.03)):
            with self.assertRaises(measure.MeasurementError):
                measure.run([sys.executable, "-c", script], size, timeout=timeout)

    def test_cli_matching_failure_and_gate_failure_are_distinct(self):
        for audio, expected in ((measure.EVENT_SECONDS[:-1], 2),
                                ([x + 0.1 for x in measure.EVENT_SECONDS], 3)):
            with mock.patch.object(measure, "probe", return_value=self.metadata()), \
                    mock.patch.object(measure, "measure_video", return_value=(measure.EVENT_SECONDS, {})), \
                    mock.patch.object(measure, "measure_audio", return_value=(audio, {})), \
                    contextlib.redirect_stdout(io.StringIO()) as output:
                code = measure.main(["recording.mkv", "--max-offset-ms", "30"])
            self.assertEqual(code, expected)
            report = json.loads(output.getvalue())
            self.assertEqual(report["valid_marker_match"], expected == 3)

    def test_cli_decoder_failure_cannot_pass(self):
        with mock.patch.object(measure, "probe", side_effect=measure.MeasurementError("decoder failed")), \
                contextlib.redirect_stderr(io.StringIO()) as output:
            self.assertEqual(measure.main(["recording.mkv"]), 1)
        self.assertFalse(json.loads(output.getvalue())["valid_marker_match"])


@unittest.skipIf(np is None, "NumPy media-detector tests are optional; pure validation tests still run")
class InjectedDetectorTests(unittest.TestCase):
    @staticmethod
    def audio_fixture(*, missing=False, duplicate=False):
        # Low-rate generated PCM exercises tone detection without FFmpeg or a
        # recording. This does not establish physical detector accuracy.
        rate = 8000
        samples = np.zeros(64 * rate, dtype=np.float32)
        for index, (onset, frequency) in enumerate(zip(measure.EVENT_SECONDS, measure.TONE_HZ)):
            if missing and index == 3:
                continue
            start = round((onset + 0.08) * rate)
            tone = 0.18 * np.sin(2 * np.pi * frequency * np.arange(round(0.2 * rate)) / rate)
            samples[start:start + len(tone)] = tone
            if duplicate and index == 3:
                samples[start + rate:start + rate + len(tone)] = tone
        frames = [{"pts_time": n / rate - 0.025, "nb_samples": 800}
                  for n in range(0, len(samples), 800)]
        return samples, frames, {"index": 1, "sample_rate": str(rate)}

    def test_tones_keep_pts_and_known_injected_delay(self):
        samples, frames, stream = self.audio_fixture()
        with mock.patch.object(measure, "probe", return_value={"frames": frames}), \
                mock.patch.object(measure, "run", return_value=samples.astype("<f4").tobytes()) as run:
            onsets, info = measure.measure_audio("test.mkv", stream, "ffmpeg", "ffprobe")
        result = measure.compare_onsets(measure.EVENT_SECONDS, onsets)
        self.assertTrue(result["valid_marker_match"])
        self.assertAlmostEqual(result["median_audio_minus_video_ms"], 55, delta=1.5)
        self.assertEqual(info["decoded_first_pts"], -0.025)
        command = run.call_args.args[0]
        self.assertIn("-copyts", command)
        self.assertNotIn("-af", command)
        self.assertEqual(run.call_args.args[1], 121 * 8000 * 4)

    def test_missing_duplicate_nonfinite_or_wrong_pcm_count_fail(self):
        for missing, duplicate in ((True, False), (False, True)):
            samples, frames, stream = self.audio_fixture(missing=missing, duplicate=duplicate)
            with mock.patch.object(measure, "probe", return_value={"frames": frames}), \
                    mock.patch.object(measure, "run", return_value=samples.astype("<f4").tobytes()):
                with self.assertRaises(measure.MeasurementError):
                    measure.measure_audio("test.mkv", stream, "ffmpeg", "ffprobe")
        samples, frames, stream = self.audio_fixture()
        for payload in (samples[:-1].astype("<f4").tobytes(),
                        np.full_like(samples, np.nan).astype("<f4").tobytes()):
            with mock.patch.object(measure, "probe", return_value={"frames": frames}), \
                    mock.patch.object(measure, "run", return_value=payload):
                with self.assertRaises(measure.MeasurementError):
                    measure.measure_audio("test.mkv", stream, "ffmpeg", "ffprobe")

    @staticmethod
    def video_fixture():
        count, frame_bytes = 64 * 20, measure.DETECT_WIDTH * measure.DETECT_HEIGHT * 3
        raw = np.zeros((count, frame_bytes // 3, 3), dtype=np.uint8)
        for onset in measure.EVENT_SECONDS:
            start = round(onset * 20)
            raw[start:start + 4, :, :] = [10, 220, 230]
        frames = [{"pts_time": n / 20 - 0.1} for n in range(count)]
        return raw.tobytes(), frames

    def test_cyan_flash_pts_and_independent_pattern(self):
        payload, frames = self.video_fixture()
        with mock.patch.object(measure, "probe", return_value={"frames": frames}), \
                mock.patch.object(measure, "run", return_value=payload):
            onsets, info = measure.measure_video("test.mkv", {"index": 0}, "ffmpeg", "ffprobe")
        self.assertTrue(measure.fingerprint(onsets, 0.04)["valid"])
        for actual, expected in zip(onsets, measure.EVENT_SECONDS):
            self.assertAlmostEqual(actual, expected - 0.1)
        self.assertEqual(info["decoded_first_pts"], -0.1)

    def test_video_byte_count_mismatch_and_no_signal_fail(self):
        payload, frames = self.video_fixture()
        for raw in (payload[:-1], bytes(len(payload))):
            with mock.patch.object(measure, "probe", return_value={"frames": frames}), \
                    mock.patch.object(measure, "run", return_value=raw):
                with self.assertRaises(measure.MeasurementError):
                    measure.measure_video("test.mkv", {"index": 0}, "ffmpeg", "ffprobe")


if __name__ == "__main__":
    unittest.main()
