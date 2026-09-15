#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Pure policy and measurement-oracle checks; not GStreamer conversion tests."""
from array import array
import copy
import importlib.util
import math
from pathlib import Path
import struct
import tempfile
import unittest

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "audio_conversion_fixture.py"
SPEC = importlib.util.spec_from_file_location("audio_conversion_fixture", MODULE_PATH)
fixture = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(fixture)


def ideal_output(config, segments):
    """Analytic expected test signals, not a signal-processing implementation."""
    frames = sum(item["frames"] for item in segments) // 4
    samples = array("f", [0.0]) * (frames * 2)
    for segment in segments:
        start, length = segment["start_frame"] // 4, segment["frames"] // 4
        kind, channel = segment["kind"], segment.get("channel", 0)
        if kind == "stopband":
            continue
        if kind == "impulse":
            for side in (0, 1):
                samples[2 * (start + length // 2) + side] = 0.125 * config["matrix"][side][channel]
            continue
        frequency = 18000 if kind == "passband" else 1000
        gain = ([sum(row) for row in config["matrix"]] if kind == "coherent" else
                [0.25 * row[channel] for row in config["matrix"]])
        for index in range(length):
            value = math.sin(2 * math.pi * frequency * index / fixture.OUTPUT_RATE)
            for side in (0, 1):
                samples[2 * (start + index) + side] = value * gain[side]
    return samples


class ConversionPolicyTests(unittest.TestCase):
    def test_windows_surround_mask_is_translated_not_copied(self):
        config = fixture.policy(8, 0x63F)
        self.assertEqual(config["gst_mask"], 0xC3F)
        self.assertEqual(config["positions"], ["FL", "FR", "FC", "LFE", "BL", "BR", "SL", "SR"])
        self.assertEqual(config["matrix"][0][3], 0)
        self.assertEqual(config["matrix"][1][3], 0)
        self.assertAlmostEqual(config["matrix"][0][0], 0.28833951691533666)
        self.assertAlmostEqual(config["matrix"][0][2], 0.20388682769488778)

    def test_supported_layouts_share_normalization(self):
        for channels, mask in ((1, 0), (1, 4), (2, 3), (6, 0x3F), (6, 0x60F), (8, 0x63F), (4, 0x107)):
            with self.subTest(mask=hex(mask)):
                config = fixture.policy(channels, mask)
                fixture.validate_policy(config)
                self.assertAlmostEqual(max(sum(abs(value) for value in row) for row in config["matrix"]), 0.9)
                self.assertTrue(all(abs(value) <= 1 for row in config["matrix"] for value in row))

    def test_invalid_or_ambiguous_layouts_fail_closed(self):
        for channels, mask in ((8, 0), (7, 0x63F), (8, 0xC3F), (1, 8), (9, 0x73F), (0, 0), (True, 0), (2, -1)):
            with self.subTest(channels=channels, mask=mask):
                with self.assertRaises(ValueError):
                    fixture.policy(channels, mask)

    def test_policy_validator_rejects_misrouting_and_nonfinite_coefficients(self):
        original = fixture.policy(8, 0x63F)
        mutations = []
        wrong = copy.deepcopy(original); wrong["gst_mask"] = 0x63F; mutations.append(wrong)
        wrong = copy.deepcopy(original); wrong["matrix"][0][3] = 0.1; mutations.append(wrong)
        wrong = copy.deepcopy(original); wrong["matrix"][0][0] = float("nan"); mutations.append(wrong)
        wrong = copy.deepcopy(original); wrong["matrix"][0][0] = float("inf"); mutations.append(wrong)
        wrong = copy.deepcopy(original); wrong["matrix"][0].pop(); mutations.append(wrong)
        wrong = copy.deepcopy(original); wrong["matrix"] = list(zip(*wrong["matrix"])); mutations.append(wrong)
        for index, config in enumerate(mutations):
            with self.subTest(mutation=index), self.assertRaises(ValueError):
                fixture.validate_policy(config)

    def test_generated_wave_header_is_extensible_float_with_windows_mask(self):
        config = fixture.policy(1, 4)
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "input.wav"
            total = fixture.generate_wav(path, config, fixture.fixture_segments(config))
            with path.open("rb") as stream:
                header = stream.read(80)
            self.assertEqual(header[:4], b"RIFF")
            self.assertEqual(struct.unpack_from("<I", header, 4)[0] + 8, path.stat().st_size)
            self.assertEqual(struct.unpack_from("<HH", header, 20), (0xFFFE, 1))
            self.assertEqual(struct.unpack_from("<I", header, 40)[0], 4)
            self.assertEqual(struct.unpack_from("<I", header, 44)[0], 3)
            self.assertEqual(struct.unpack_from("<I", header, 68)[0], total)
            with self.assertRaises(FileExistsError):
                fixture.generate_wav(path, config, fixture.fixture_segments(config))


class ConversionMeasurementTests(unittest.TestCase):
    def setUp(self):
        self.config = fixture.policy(8, 0x63F)
        self.segments = fixture.fixture_segments(self.config)
        self.samples = ideal_output(self.config, self.segments)

    def test_analytic_reference_fixture_passes(self):
        report = fixture.analyze_samples(self.samples, self.config, self.segments)
        self.assertTrue(report["passed"])
        self.assertEqual(len(report["segments"]), 19)

    def test_right_left_swap_is_detected(self):
        swapped = array("f", self.samples)
        swapped[0::2], swapped[1::2] = self.samples[1::2], self.samples[0::2]
        self.assertFalse(fixture.analyze_samples(swapped, self.config, self.segments)["passed"])

    def test_alias_leakage_is_detected(self):
        segment = self.segments[-1]
        start, frames = segment["start_frame"] // 4, segment["frames"] // 4
        for index in range(frames):
            self.samples[(start + index) * 2] = 0.01 * math.sin(2 * math.pi * 18000 * index / fixture.OUTPUT_RATE)
        self.assertFalse(fixture.analyze_samples(self.samples, self.config, self.segments)["passed"])

    def test_impulse_shift_is_detected(self):
        segment = self.segments[8]
        center = (segment["start_frame"] + segment["frames"] // 2) // 4
        self.samples[(center + 3) * 2] = self.samples[center * 2]
        self.samples[center * 2] = 0
        self.assertFalse(fixture.analyze_samples(self.samples, self.config, self.segments)["passed"])

    def test_neighboring_tone_filter_tails_do_not_fail_impulse_routing(self):
        # The first/last impulse slots touch tone segments. A real resampler
        # rings at those edges; unrelated edge energy must not mimic cross-talk.
        first = self.segments[8]["start_frame"] // 4
        last = (self.segments[15]["start_frame"] + self.segments[15]["frames"]) // 4 - 1
        self.samples[first * 2 + 1] = 0.000713
        self.samples[last * 2] = 0.001542
        self.assertTrue(fixture.analyze_samples(self.samples, self.config, self.segments)["passed"])

    def test_impulse_center_cross_talk_is_detected(self):
        segment = self.segments[8]
        center = (segment["start_frame"] + segment["frames"] // 2) // 4
        self.samples[center * 2 + 1] = 0.001
        self.assertFalse(fixture.analyze_samples(self.samples, self.config, self.segments)["passed"])

    def test_clipping_and_invalid_output_are_detected(self):
        self.samples[0] = 1.0
        self.assertFalse(fixture.analyze_samples(self.samples, self.config, self.segments)["passed"])
        self.samples[0] = float("nan")
        with self.assertRaises(ValueError):
            fixture.analyze_samples(self.samples, self.config, self.segments)
        with self.assertRaises(ValueError):
            fixture.analyze_samples(array("f"), self.config, self.segments)


if __name__ == "__main__":
    unittest.main()
