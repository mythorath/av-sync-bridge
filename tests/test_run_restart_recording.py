# SPDX-License-Identifier: GPL-2.0-or-later
import sys
import argparse
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import run_restart_recording as runner


class RestartProofTests(unittest.TestCase):
    def setUp(self):
        self.previous = dict(fixture_id=1, fps=60, delay_ms=2000, generation=2**64-1,
                             epoch_ns=10_000_000_000, ready_ns=10_010_000_000)
        self.barrier = dict(fixture_id=1, marker=4, generation=2**64-1,
                            barrier_ns=16_790_000_000, presentation_start_ns=18_700_000_000,
                            video_frames=408, audio_blocks=680)
        self.successor = dict(fixture_id=2, fps=60, delay_ms=2000, generation=1,
                              epoch_ns=16_820_000_000, ready_ns=16_850_000_000)

    def proof(self):
        return runner.validate_successor(self.previous, self.barrier, self.successor,
                                         16_800_000_000, 16_810_000_000)

    def test_valid_and_private_identity_not_reported(self):
        report = self.proof()
        self.assertTrue(report["fresh_generation"])
        self.assertEqual(report["replacement_margin_ms"], 1850)
        self.assertNotIn("generation", report)
        self.assertNotIn("epoch_ns", report)

    def test_incomplete_queued_media(self):
        for field, value in (("video_frames", 407), ("audio_blocks", 673)):
            saved = self.barrier[field]
            self.barrier[field] = value
            with self.assertRaises(ValueError): self.proof()
            self.barrier[field] = saved

    def test_late_replacement_rejected(self):
        self.successor["ready_ns"] = 18_200_000_000
        with self.assertRaises(ValueError): self.proof()

    def test_reused_generation_rejected(self):
        self.successor["generation"] = self.previous["generation"]
        with self.assertRaises(ValueError): self.proof()

    def test_clock_reversal_rejected(self):
        self.successor["epoch_ns"] = 16_800_000_000
        with self.assertRaises(ValueError): self.proof()

    def test_wrong_future_event_rejected(self):
        self.barrier["presentation_start_ns"] += 1
        with self.assertRaises(ValueError): self.proof()

    def test_log_strict_integer_and_unique_record(self):
        line = "SYNTHETIC generation=18446744073709551615 epoch_ns=10 size=640x360 path=/private/example with spaces\n"
        fields = runner.record_fields(line, "SYNTHETIC")
        self.assertEqual(fields, {"generation": 2**64-1, "epoch_ns": 10})
        for invalid in (line + line, line.replace("epoch_ns=10", "epoch_ns=-10"),
                        line.replace("epoch_ns=10", "epoch_ns=9223372036854775808"),
                        line.replace("epoch_ns=10", "epoch_ns=10 epoch_ns=10")):
            with self.assertRaises(ValueError): runner.record_fields(invalid, "SYNTHETIC")


class PrivateDisplayTests(unittest.TestCase):
    def test_display_number_is_a_bounded_exact_record(self):
        self.assertEqual(runner.parse_display_number(b"0\n"), ":0")
        self.assertEqual(runner.parse_display_number(b"123\n"), ":123")
        self.assertEqual(runner.parse_display_number(b"65535\n"), ":65535")
        for value in (b"", b"\n", b"-1\n", b"1\r\n", b"1\n2\n", b" 1\n", b"65536\n",
                      b"1", b":1\n", b"\xff\n", b"123456789\n"):
            with self.subTest(value=value), self.assertRaises(ValueError):
                runner.parse_display_number(value)

    def test_pipe_startup_accepts_split_number_without_live_display(self):
        child = Mock()
        child.poll.return_value = None
        with patch.object(runner.select, "select", return_value=([7], [], [])), \
                patch.object(runner.os, "read", side_effect=[b"12", b"3\n"]):
            self.assertEqual(runner.wait_display_fd(7, child, 1), ":123")

    def test_pipe_eof_exit_and_size_fail_closed(self):
        for chunks, expected in (([b""], RuntimeError), ([b"x" * 32, b"x"], ValueError),
                                 ([b"1\n2\n"], ValueError)):
            child = Mock()
            child.poll.return_value = None
            with self.subTest(chunks=chunks), \
                    patch.object(runner.select, "select", return_value=([7], [], [])), \
                    patch.object(runner.os, "read", side_effect=chunks), self.assertRaises(expected):
                runner.wait_display_fd(7, child, 1)
        child.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            runner.wait_display_fd(7, child, 1)

    def test_pipe_wait_deadline_is_bounded(self):
        child = Mock()
        child.poll.return_value = None
        with patch.object(runner.time, "monotonic", side_effect=[10, 12]), \
                self.assertRaises(TimeoutError):
            runner.wait_display_fd(7, child, 1)

    def test_owned_display_cleanup_survives_harness_failure(self):
        for cleanup_failure in (False, True):
            with self.subTest(cleanup_failure=cleanup_failure), tempfile.TemporaryDirectory() as temporary:
                args = argparse.Namespace(build_dir=Path("/test/build"), obs_plugins=Path("/test/plugins"),
                                          obs_data=Path("/test/data"))
                display, harness = Mock(), Mock()
                display.poll.return_value = None
                harness.poll.return_value = 1
                with patch.object(runner.subprocess, "Popen", side_effect=[display, harness]) as popen, \
                        patch.object(runner.os, "pipe", return_value=(101, 102)), \
                        patch.object(runner.os, "close") as close, \
                        patch.object(runner, "wait_display_fd", return_value=":88"), \
                        patch.object(runner, "wait_marker", side_effect=RuntimeError("harness_failed")), \
                        patch.object(runner, "stop_child", side_effect=[RuntimeError("stop_failed"), None]
                                     if cleanup_failure else None) as stop, \
                        patch.dict(os.environ, {"DISPLAY": ":original", "XAUTHORITY": "private-login-cookie"}), \
                        self.assertRaises(RuntimeError):
                    runner.run_case(args, "graceful", Path(temporary) / "case")
                self.assertEqual([call.args[0] for call in stop.call_args_list], [harness, display])
                xvfb_call, obs_call = popen.call_args_list
                self.assertEqual(xvfb_call.args[0][0], "Xvfb")
                self.assertEqual(xvfb_call.kwargs["pass_fds"], (102,))
                self.assertEqual(obs_call.args[0][0], str(args.build_dir / "avsync-obs-smoke"))
                self.assertEqual(obs_call.kwargs["env"]["DISPLAY"], ":88")
                self.assertNotIn("XAUTHORITY", obs_call.kwargs["env"])
                self.assertEqual(sorted(call.args[0] for call in close.call_args_list), [101, 102])


if __name__ == "__main__":
    unittest.main()
