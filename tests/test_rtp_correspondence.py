#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
import importlib.util
import json
from pathlib import Path
import unittest

SPEC = importlib.util.spec_from_file_location("rtp_correspondence",
    Path(__file__).resolve().parents[1] / "tools" / "check_rtp_correspondence.py")
checker = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(checker)


class RtpCorrespondenceTests(unittest.TestCase):
    def summary(self, delta=4800, sr_rtp=10000, pts_error_ns=0):
        sr_seconds = 1000
        last_pts = sr_seconds * 1_000_000_000 + delta * 1_000_000_000 // 48000
        return {"schema": 1, "status": "rtp_output_observed_unverified", "desktop_only": True,
                "clock_usable": True, "resets": 0, "mapped_packets": 200,
                "rtp_packets_at_output": 800, "rtcp_packets_at_output": 5, "sender_reports": 5,
                "first_rtp_pts_ns": last_pts - 2_000_000_000,
                "last_rtp_pts_ns": last_pts + pts_error_ns,
                "last_rtp_timestamp": (sr_rtp + delta) & ((1 << 32) - 1),
                "last_sr_clock_32_32": sr_seconds << 32, "last_sr_rtp_timestamp": sr_rtp}

    def test_exact_correspondence_and_no_absolute_output(self):
        report = checker.check_summary(self.summary())
        self.assertTrue(report["passed"])
        self.assertTrue(report["valid_evidence"])
        self.assertEqual(report["exact_error_ns"], {"numerator": 0, "denominator": 1})
        for key in ("last_rtp_pts_ns", "first_rtp_pts_ns", "last_sr_clock_32_32", "ssrc"):
            self.assertNotIn(key, report)
        self.assertFalse(report["receiver_verified"])
        self.assertFalse(report["clock_accuracy_verified"])
        self.assertFalse(report["continuity_verified"])

    def test_positive_and_negative_rtp_wrap(self):
        for delta, sr in ((4800, (1 << 32) - 2400), (-4800, 2400), (0, 0), (1, (1 << 32) - 1)):
            report = checker.check_summary(self.summary(delta, sr))
            self.assertTrue(report["passed"])
            self.assertEqual(report["rtp_delta_samples"], delta)

    def test_injected_offset_and_exact_sample_threshold(self):
        for offset in (-20833, 20833, 0):
            self.assertTrue(checker.check_summary(self.summary(pts_error_ns=offset))["passed"])
        for offset in (-20834, 20834, 1_000_000):
            report = checker.check_summary(self.summary(pts_error_ns=offset))
            self.assertTrue(report["valid_evidence"])
            self.assertFalse(report["passed"])
            self.assertEqual(report["reconstructed_minus_rtp_pts_ns"], -offset)

    def test_fractional_ntp_tick_and_exact_tolerance_edge(self):
        summary = self.summary()
        summary["last_sr_clock_32_32"] += 1
        report = checker.check_summary(summary)
        self.assertTrue(report["passed"])
        self.assertAlmostEqual(report["reconstructed_minus_rtp_pts_ns"], 1_000_000_000 / (1 << 32))
        self.assertNotEqual(report["exact_error_ns"]["numerator"], 0)
        # 20833 ns + one NTP tick is still below one sample; two ticks exceed it.
        for ticks, expected in ((1, True), (2, False)):
            summary = self.summary(pts_error_ns=-20833)
            summary["last_sr_clock_32_32"] += ticks
            self.assertEqual(checker.check_summary(summary)["passed"], expected)

    def test_rate_mismatch_is_not_hidden_by_wrap_or_offset_fit(self):
        for ppm in (100, 500, -100, -500):
            summary = self.summary(delta=48000)
            drift_ns = ppm * 1000  # Over a one-second RTP interval.
            summary["last_sr_clock_32_32"] += drift_ns * (1 << 32) // 1_000_000_000
            report = checker.check_summary(summary)
            self.assertTrue(report["valid_evidence"])
            self.assertFalse(report["passed"])
            self.assertAlmostEqual(report["reconstructed_minus_rtp_pts_ns"], drift_ns, delta=0.24)

    def test_half_wrap_and_out_of_window_rejected(self):
        for last, sr in ((1 << 31, 0), (0, 1 << 31), (31*48000, 0), (0, 31*48000)):
            with self.assertRaises(checker.EvidenceError):
                checker.signed_rtp_delta(last, sr)
        self.assertEqual(checker.signed_rtp_delta(30*48000, 0), 30*48000)

    def test_missing_invalid_and_weak_evidence(self):
        mutations = {"sender_reports": 0, "rtcp_packets_at_output": 0, "rtp_packets_at_output": 1,
                     "mapped_packets": 0, "resets": 1, "clock_usable": False, "desktop_only": False,
                     "status": "waiting_no_rtp", "schema": 2, "last_sr_clock_32_32": 0,
                     "last_rtp_timestamp": 1 << 32, "last_sr_rtp_timestamp": -1,
                     "last_rtp_pts_ns": 1 << 63, "first_rtp_pts_ns": 0}
        for field, value in mutations.items():
            with self.subTest(field=field):
                summary = self.summary()
                summary[field] = value
                with self.assertRaises(checker.EvidenceError):
                    checker.check_summary(summary)
        for field in ("resets", "last_sr_clock_32_32", "last_rtp_timestamp", "last_rtp_pts_ns"):
            for invalid in (None, True, "12", 12.0):
                summary = self.summary()
                summary[field] = invalid
                with self.assertRaises(checker.EvidenceError):
                    checker.check_summary(summary)

    def test_incompatible_clock_era_span_and_counts(self):
        for field, value in (("last_sr_clock_32_32", 2 << 32),
                             ("first_rtp_pts_ns", self.summary()["last_rtp_pts_ns"]),
                             ("first_rtp_pts_ns", self.summary()["last_rtp_pts_ns"]-31_000_000_000),
                             ("sender_reports", 6), ("last_sr_clock_32_32", 1 << 64)):
            summary = self.summary()
            summary[field] = value
            with self.assertRaises(checker.EvidenceError):
                checker.check_summary(summary)

    def test_json_exactly_one_summary_and_no_duplicate_keys(self):
        encoded = json.dumps(self.summary())
        self.assertEqual(checker.parse_sender_json(encoded), self.summary())
        self.assertEqual(checker.parse_sender_json("A diagnostic line\n" + encoded), self.summary())
        for bad in (encoded+"\n"+encoded, "[]", "{}\n{}", '{"schema":1,"schema":1}',
                    '{"value":NaN}', "prefix\n{broken}", '{"x":'+"["*1500+"]"*1500+"}"):
            with self.assertRaises(checker.EvidenceError):
                checker.parse_sender_json(bad)


if __name__ == "__main__":
    unittest.main()
