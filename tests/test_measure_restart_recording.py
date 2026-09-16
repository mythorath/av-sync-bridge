#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Generated analyzer fixtures only; no OBS, network, devices, or media tools."""
import contextlib
import importlib.util
import io
import json
import math
from pathlib import Path
import sys
import tempfile
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
try:
    SPEC = importlib.util.spec_from_file_location("measure_restart_recording", TOOLS / "measure_restart_recording.py")
    measure = importlib.util.module_from_spec(SPEC)
    SPEC.loader.exec_module(measure)
finally:
    sys.path.pop(0)


class RestartMeasurementTests(unittest.TestCase):
    before = {"fixture_id": 1, "epoch_ns": 1_000_000_000_000, "delay_ms": 2000, "fps": 60}
    after = {"fixture_id": 2, "epoch_ns": 1_007_000_000_000, "delay_ms": 2000, "fps": 60}

    @staticmethod
    def log(header):
        return "SYNTHETIC generation=123 " + " ".join(f"{key}={value}" for key, value in header.items()) + " path=/private/fixture\n"

    def events(self, audio_ms=0):
        times = [(stamp - self.before["epoch_ns"]) / 1e9 - .2
                 for stamp in measure.expected_times(self.before, self.after)]
        return ([(role, stamp) for (role, _), stamp in zip(measure.EXPECTED_IDS, times)],
                [(role, event, stamp + audio_ms / 1000)
                 for (role, event), stamp in zip(measure.EXPECTED_IDS, times)])

    def rows(self, offset_ns=0, missing=None, extra=None):
        starts = list(measure.expected_times(self.before, self.after))
        if missing is not None:
            del starts[missing]
        if extra is not None:
            starts.append(self.before["epoch_ns"] + extra)
        for start in sorted(starts):
            for delta in range(-5_000_000, 45_000_000, 1_000_000):
                yield {"timestamp_ns": start + delta + offset_ns, "frames": 48,
                       "rms": .1 if 0 <= delta < 40_000_000 else 0}

    def test_headers_exact_roles_bounded_fields_no_duplicate(self):
        self.assertEqual(measure.parse_header(self.log(self.before), 1), self.before)
        self.assertEqual(measure.parse_header(self.log(self.after), 2), self.after)
        invalid = ("", self.log(self.before) * 2, self.log(self.after),
                   self.log(dict(self.before, fps=59)), self.log(dict(self.before, delay_ms=1975)),
                   self.log(dict(self.before, epoch_ns=-1)), self.log(dict(self.before, fixture_id="1x")),
                   self.log(self.before).replace("path=", "fps=60 path="), "x" * 65_537)
        for text in invalid:
            with self.subTest(text=text[:80]), self.assertRaises(measure.MeasurementError):
                measure.parse_header(text, 1)

    def test_headers_wrong_epoch_order_span_and_overflow_fail(self):
        for header in (dict(self.after, epoch_ns=self.before["epoch_ns"]),
                       dict(self.after, epoch_ns=self.before["epoch_ns"] - 1),
                       dict(self.after, epoch_ns=self.before["epoch_ns"] + 1),
                       dict(self.after, epoch_ns=self.before["epoch_ns"] + 121_000_000_000),
                       dict(self.after, epoch_ns=measure.MAX_NS), dict(self.after, epoch_ns=True)):
            with self.subTest(header=header), self.assertRaises(measure.MeasurementError):
                measure.expected_times(self.before, header)

    def test_exact_nine_ids_and_original_timing(self):
        video, audio = self.events(-15.333)
        report = measure.analyze_encoded(video, audio, self.before, self.after)
        self.assertTrue(report["valid_marker_match"])
        self.assertTrue(report["timing_gate"]["passed"])
        self.assertEqual(report["expected_event_ids"], ["A1", "A2", "A3", "B1", "B2", "B3", "B4", "B5", "B6"])
        self.assertAlmostEqual(report["roles"][1]["median_audio_minus_video_ms"], -15.333)

    def test_missing_duplicate_old_replay_and_wrong_identity_fail(self):
        video, audio = self.events()
        old = (1, 4, 8.5)
        variants = [audio[:-1], audio + [(2, 6, 30)], audio[:3] + [old] + audio[3:],
                    [(2, 1, audio[0][-1])] + audio[1:],
                    [(1, 4, audio[0][-1])] + audio[1:]]
        for wrong in variants:
            with self.subTest(audio=wrong):
                self.assertFalse(measure.analyze_encoded(video, wrong, self.before, self.after)["valid_marker_match"])
        self.assertFalse(measure.analyze_encoded(video[:3] + [(1, 8.5)] + video[3:], audio,
                                                self.before, self.after)["valid_marker_match"])

    def test_generation_swap_and_identity_swap_cannot_pair(self):
        video, audio = self.events()
        bad_video = [(3 - role, t) for role, t in video]
        self.assertFalse(measure.analyze_encoded(bad_video, audio, self.before, self.after)["valid_marker_match"])
        bad_audio = list(audio)
        bad_audio[0] = (*audio[1][:2], audio[0][-1])
        bad_audio[1] = (*audio[0][:2], audio[1][-1])
        self.assertFalse(measure.analyze_encoded(video, bad_audio, self.before, self.after)["valid_marker_match"])

    def test_cycle_shift_and_mixed_epochs_cannot_pass_relative_av(self):
        video, audio = self.events()
        shifted_video = [(role, t + (20 if role == 2 else 0)) for role, t in video]
        shifted_audio = [(role, event, t + (20 if role == 2 else 0)) for role, event, t in audio]
        self.assertFalse(measure.analyze_encoded(shifted_video, shifted_audio, self.before, self.after)["valid_marker_match"])
        self.assertFalse(measure.analyze_encoded(video, audio, self.before,
                                                dict(self.after, epoch_ns=self.after["epoch_ns"] + 20_000_000_000))["valid_marker_match"])
        # A common entire-file offset is unknowable from relative encoded A/V;
        # the independently required raw absolute-time gate catches that error.
        common_video = [(role, t + 20) for role, t in video]
        common_audio = [(role, event, t + 20) for role, event, t in audio]
        self.assertTrue(measure.analyze_encoded(common_video, common_audio, self.before, self.after)["timing_gate"]["passed"])
        self.assertFalse(measure.analyze_mixer(self.rows(20_000_000_000), self.before, self.after)["timing_gate"]["passed"])

    def test_each_role_median_and_every_event_have_independent_gates(self):
        for role in (1, 2):
            video, audio = self.events()
            changed = [(r, e, t + (.020 if r == role else 0)) for r, e, t in audio]
            report = measure.analyze_encoded(video, changed, self.before, self.after)
            self.assertTrue(report["valid_marker_match"])
            self.assertFalse(report["timing_gate"]["passed"])
        video, audio = self.events()
        audio[5] = (*audio[5][:2], audio[5][-1] + .034)
        report = measure.analyze_encoded(video, audio, self.before, self.after)
        self.assertTrue(report["valid_marker_match"])
        self.assertFalse(report["timing_gate"]["passed"])

    def test_negative_encoded_origin_is_not_rebased(self):
        video, audio = self.events(-10)
        video = [(role, t - 3) for role, t in video]
        audio = [(role, event, t - 3) for role, event, t in audio]
        report = measure.analyze_encoded(video, audio, self.before, self.after)
        self.assertTrue(report["timing_gate"]["passed"])
        self.assertAlmostEqual(report["audio_minus_video_ms"][0], -10)

    def test_invalid_encoded_times_are_not_coerced(self):
        video, audio = self.events()
        for value in (float("nan"), float("inf"), True, audio[0][-1]):
            bad = list(audio)
            bad[1] = (*bad[1][:2], value)
            with self.subTest(value=value), self.assertRaises(measure.MeasurementError):
                measure.analyze_encoded(video, bad, self.before, self.after)

    def test_full_raw_trace_offsets_and_missing_extra_replay(self):
        for offset, passed in ((0, True), (-500_000, True), (2_000_000, True), (2_001_000, False)):
            report = measure.analyze_mixer(self.rows(offset), self.before, self.after)
            self.assertTrue(report["valid_marker_match"])
            self.assertEqual(report["timing_gate"]["passed"], passed)
        for rows in (self.rows(missing=2), self.rows(extra=8_700_000_000), self.rows(extra=1_000_000_000),
                     self.rows(extra=26_000_000_000)):
            report = measure.analyze_mixer(rows, self.before, self.after)
            self.assertFalse(report["valid_marker_match"])
            self.assertIsNone(report["timing_gate"]["passed"])

    def test_raw_mixed_epochs_and_invalid_rows(self):
        self.assertFalse(measure.analyze_mixer(self.rows(), self.before,
                         dict(self.after, epoch_ns=self.after["epoch_ns"] + 100_000_000))["timing_gate"]["passed"])
        sample = {"timestamp_ns": 1, "frames": 48, "rms": .1}
        for rows in ([sample, sample], [dict(sample, rms=float("nan"))], [dict(sample, frames=49)],
                     [dict(sample, timestamp_ns=-1)], [sample, dict(sample, timestamp_ns=131_000_000_002)]):
            with self.subTest(rows=rows), self.assertRaises(measure.MeasurementError):
                measure.analyze_mixer(rows, self.before, self.after)
        with mock.patch.object(measure, "MAX_ROWS", 1), self.assertRaises(measure.MeasurementError):
            measure.analyze_mixer(self.rows(), self.before, self.after)

    @staticmethod
    def tone(frequencies, frames=1920):
        return [sum(.2 / len(frequencies) * math.sin(2 * math.pi * hz * n / 48000)
                    for hz in frequencies) for n in range(frames)]

    def test_independent_tone_identity_all_twelve_and_queued_a4(self):
        for frequency, identity in measure.TONE_IDS.items():
            with self.subTest(frequency=frequency):
                self.assertEqual(measure.classify_tone(self.tone([frequency])), identity)

    def test_unknown_mixed_weak_and_invalid_tones_rejected(self):
        for samples in (self.tone([2110]), self.tone([660, 2860]), [0.] * 1920,
                        [float("nan")] * 1920, self.tone([660], 100)):
            with self.subTest(size=len(samples)), self.assertRaises(measure.MeasurementError):
                measure.classify_tone(samples)

    def test_color_identity_white_cyan_and_unknown(self):
        pixels = measure.WIDTH * measure.HEIGHT
        self.assertEqual(measure.classify_picture(bytes([250, 250, 250]) * pixels)[1], 1)
        self.assertEqual(measure.classify_picture(bytes([0, 253, 255]) * pixels)[1], 2)
        self.assertEqual(measure.classify_picture(bytes([255, 0, 0]) * pixels)[1], 0)

    def video_fixture(self, rogue=None, quantized=False):
        times = [round(n / 60, 3) if quantized else n / 60 for n in range(120)]
        colors = []
        for n in range(120):
            color = (250, 250, 250) if 12 <= n < 18 else (0, 253, 255) if 54 <= n < 60 else (0, 0, 0)
            if rogue and 30 <= n < 36:
                color = rogue
            colors.append(bytes(color) * measure.WIDTH * measure.HEIGHT)
        return times, b"".join(colors)

    def test_video_region_identity_and_millisecond_pts(self):
        for quantized in (False, True):
            events = measure.detect_video(*self.video_fixture(quantized=quantized))
            self.assertEqual([role for role, _ in events], [1, 2])
            self.assertAlmostEqual(events[0][-1], .2)
            self.assertAlmostEqual(events[1][-1], .9)

    def test_unknown_bright_region_is_never_discarded(self):
        with self.assertRaises(measure.MeasurementError):
            measure.detect_video(*self.video_fixture(rogue=(255, 0, 0)))

    def test_full_audio_regions_are_identified_without_assumed_order(self):
        samples = [0.] * 48000
        samples[9600:11520] = self.tone([2860])
        samples[28800:30720] = self.tone([660])
        events = measure.detect_audio(samples, [0.], [48000], [0])
        self.assertEqual([event[:2] for event in events], [(2, 1), (1, 1)])
        self.assertAlmostEqual(events[0][-1], .2)
        self.assertAlmostEqual(events[1][-1], .6)
        samples[19200:21120] = self.tone([2110])
        with self.assertRaises(measure.MeasurementError):
            measure.detect_audio(samples, [0.], [48000], [0])

    def test_short_unexpected_click_not_silently_discarded(self):
        samples = [0.] * 48000
        samples[9600:11520] = self.tone([660])
        samples[19200:19296] = [.2] * 96
        with self.assertRaises(measure.MeasurementError):
            measure.detect_audio(samples, [0.], [48000], [0])

    def invoke(self, directory, *, audio_ms=0, raw_offset_ns=0, decode_error=None):
        before, after, trace = directory / "before.log", directory / "after.log", directory / "mix.csv"
        before.write_text(self.log(self.before), encoding="utf-8")
        after.write_text(self.log(self.after), encoding="utf-8")
        trace.write_text("timestamp_ns,frames,rms\n" + "".join(
            f"{r['timestamp_ns']},{r['frames']},{r['rms']}\n" for r in self.rows(raw_offset_ns)), encoding="utf-8")
        output = io.StringIO()
        with mock.patch.object(measure, "measure_recording", side_effect=decode_error,
                               return_value=(*self.events(audio_ms), {"fixture": True})), contextlib.redirect_stdout(output):
            code = measure.main(["--recording", str(directory / "private.mkv"), "--predecessor-log", str(before),
                                 "--successor-log", str(after), "--mixer-trace", str(trace)])
        return code, json.loads(output.getvalue()), output.getvalue()

    def test_cli_requires_both_encoded_and_raw_gates_and_preserves_privacy(self):
        for encoded_ms, raw_ns, expected in ((0, 0, 0), (20, 0, 3), (0, 3_000_000, 3)):
            with self.subTest(encoded=encoded_ms, raw=raw_ns), tempfile.TemporaryDirectory() as temporary:
                code, report, serialized = self.invoke(Path(temporary), audio_ms=encoded_ms, raw_offset_ns=raw_ns)
                self.assertEqual(code, expected)
                self.assertEqual(report["passed"], expected == 0)
                for forbidden in (temporary, "epoch_ns", str(self.before["epoch_ns"]), "generation=", "/private/fixture"):
                    self.assertNotIn(forbidden, serialized)

    def test_decoder_errors_do_not_disclose_input_path(self):
        with tempfile.TemporaryDirectory() as temporary:
            code, report, serialized = self.invoke(Path(temporary), decode_error=measure.bounded.MeasurementError(
                "decoder cannot open /private/secret.mkv"))
            self.assertEqual(code, 1)
            self.assertFalse(report["passed"])
            self.assertNotIn("secret", serialized)

    def test_decode_calls_are_bounded_and_no_external_tools_in_unit_test(self):
        metadata = {"format": {"duration": "29"}, "streams": [
            {"index": 0, "codec_type": "video", "width": 640, "height": 360},
            {"index": 1, "codec_type": "audio", "sample_rate": "48000"}]}
        vframes = [{"pts_time": 0}, {"pts_time": .017}]
        aframes = [{"pts_time": 0, "nb_samples": 1024}, {"pts_time": .021, "nb_samples": 1024}]
        with mock.patch.object(measure.bounded, "probe", side_effect=[metadata, {"frames": vframes}, {"frames": aframes}]), \
                mock.patch.object(measure.bounded, "run", side_effect=[b"x", b"\0" * 8192]) as run, \
                mock.patch.object(measure, "detect_video", return_value=[]), \
                mock.patch.object(measure, "detect_audio", return_value=[]):
            measure.measure_recording("unused", "ffmpeg", "ffprobe")
        self.assertEqual(run.call_count, 2)
        self.assertEqual(run.call_args_list[0].args[1], 14_401 * measure.WIDTH * measure.HEIGHT * 3)
        self.assertEqual(run.call_args_list[1].args[1], 121 * 48000 * 4)
        for call in run.call_args_list:
            self.assertIn("-copyts", call.args[0])
            self.assertIn("121", call.args[0])


if __name__ == "__main__":
    unittest.main()
