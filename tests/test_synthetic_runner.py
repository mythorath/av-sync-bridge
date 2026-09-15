#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mocked media children and safe CLI parsing subprocesses; no OBS or devices."""

import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock

MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "run_synthetic_suite.py"
SPEC = importlib.util.spec_from_file_location("run_synthetic_suite", MODULE_PATH)
runner = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(runner)


def mute_report(desktop_ms=0.0, mic_ms=0.0):
    video = [3.0, 4.35, 6.1, 8.7, 12.15, 16.8]
    return {"video_onsets_seconds": video, "video_fingerprint": {"valid": True},
            "tracks": [{"onsets_seconds": [value + desktop_ms / 1000 for value in video],
                        "fingerprint": {"valid": True}},
                       {"onsets_seconds": [value + mic_ms / 1000 for index, value in enumerate(video)
                                            if index != 2]}]}


class MuteFixtureTests(unittest.TestCase):
    def test_exact_expected_suppression_passes(self):
        report = runner.check_mute_case(mute_report(8, -12))
        self.assertTrue(report["passed"])
        self.assertEqual(report["expected_suppressed_mic_event_one_based"], 3)

    def test_each_track_must_pass_median_gate(self):
        # These satisfy the per-marker 33.333 ms bound but not the median bound.
        for desktop, mic in ((30, 0), (-30, 0), (0, 30), (0, -30)):
            with self.subTest(desktop=desktop, mic=mic):
                self.assertFalse(runner.check_mute_case(mute_report(desktop, mic))["passed"])

    def test_outlier_cannot_hide_behind_good_median(self):
        for index in (0, 1):
            report = mute_report()
            report["tracks"][index]["onsets_seconds"][-1] += 0.040
            with self.subTest(track=index):
                self.assertFalse(runner.check_mute_case(report)["passed"])

    def test_wrong_suppressed_event_is_rejected(self):
        report = mute_report()
        report["tracks"][1]["onsets_seconds"] = [value for index, value in enumerate(report["video_onsets_seconds"])
                                                if index != 3]
        self.assertFalse(runner.check_mute_case(report)["passed"])

    def test_missing_extra_and_invalid_fixture_are_rejected(self):
        reports = []
        report = mute_report()
        report["tracks"][1]["onsets_seconds"] = report["video_onsets_seconds"].copy()
        reports.append(report)
        report = mute_report()
        report["tracks"][0]["onsets_seconds"].pop()
        reports.append(report)
        report = mute_report()
        report["tracks"][1]["onsets_seconds"].pop()
        reports.append(report)
        report = mute_report()
        report["video_fingerprint"]["valid"] = False
        reports.append(report)
        report = mute_report()
        report["tracks"][0]["fingerprint"]["valid"] = False
        reports.append(report)
        reports.extend([{}, {"video_onsets_seconds": [1]}])
        for index, report in enumerate(reports):
            with self.subTest(fixture=index):
                self.assertFalse(runner.check_mute_case(report)["passed"])


class ChildCleanupTests(unittest.TestCase):
    def setUp(self):
        self.child = mock.Mock(pid=43210)
        self.child.poll.return_value = None
        self.child.wait.return_value = 0
        # Mock POSIX-only APIs and constants so these pure tests also run on Windows.
        self.signals = SimpleNamespace(SIGCONT=18, SIGTERM=15, SIGKILL=9)
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.stack.enter_context(mock.patch.object(runner, "signal", self.signals))
        self.getpgid = self.stack.enter_context(mock.patch.object(runner.os, "getpgid", return_value=self.child.pid, create=True))
        self.killpg = self.stack.enter_context(mock.patch.object(runner.os, "killpg", create=True))

    def test_graceful_cleanup_continues_stopped_child_before_terminate(self):
        runner.stop_child(self.child)
        self.assertEqual(self.killpg.call_args_list,
                         [mock.call(self.child.pid, self.signals.SIGCONT),
                          mock.call(self.child.pid, self.signals.SIGTERM)])
        self.child.wait.assert_called_once_with(timeout=5)

    def test_timeout_escalates_only_known_group(self):
        self.child.wait.side_effect = [subprocess.TimeoutExpired("fixture", 5), 0]
        runner.stop_child(self.child)
        self.assertEqual(self.killpg.call_args_list[-1], mock.call(self.child.pid, self.signals.SIGKILL))
        self.assertEqual(self.child.wait.call_count, 2)

    def test_exited_and_missing_child_are_not_signaled(self):
        runner.stop_child(None)
        self.child.poll.return_value = 0
        runner.stop_child(self.child)
        self.getpgid.assert_not_called()
        self.killpg.assert_not_called()

    def test_unexpected_process_group_is_not_signaled(self):
        self.getpgid.return_value = self.child.pid + 1
        with self.assertRaisesRegex(RuntimeError, "unexpected child process group"):
            runner.stop_child(self.child)
        self.killpg.assert_not_called()

    def test_exit_race_during_group_lookup_is_normal(self):
        self.getpgid.side_effect = ProcessLookupError()
        runner.stop_child(self.child)
        self.killpg.assert_not_called()
        self.child.wait.assert_called_once_with(timeout=5)

    def test_exit_race_during_signal_is_normal(self):
        self.killpg.side_effect = ProcessLookupError()
        runner.stop_child(self.child)
        self.child.wait.assert_called_once_with(timeout=5)


class RunnerFinallyTests(unittest.TestCase):
    def run_mock_case(self, directory, case="baseline", analyzer_code=0, analyzer_report=None,
                      analyzer_error=None, cleanup_effect=None, raw_codes=None):
        args = SimpleNamespace(build_dir=directory / "build", obs_plugins=directory / "plugins",
                               obs_data=directory / "data", cycles=1)
        harness, sender = mock.Mock(name="harness"), mock.Mock(name="sender")
        harness.wait.return_value = sender.wait.return_value = 0
        result = SimpleNamespace(returncode=analyzer_code, stdout=json.dumps(analyzer_report or {}), stderr="")
        raw_count = 1 if case in ("mute", "rapid-mute") else 2
        raw_codes = (0,) * raw_count if raw_codes is None else raw_codes
        self.assertEqual(len(raw_codes), raw_count)
        responses = [result] + [SimpleNamespace(returncode=code, stdout=json.dumps({"passed": code == 0}), stderr="")
                                for code in raw_codes]
        with mock.patch.object(runner.subprocess, "Popen", side_effect=[harness, sender]) as launch, \
                mock.patch.object(runner, "wait_marker"), \
                mock.patch.object(runner.subprocess, "run", side_effect=analyzer_error or responses) as analyze, \
                mock.patch.object(runner, "stop_child", side_effect=cleanup_effect) as stop:
            try:
                return runner.run_case(args, case, directory / "case")
            finally:
                self.assertEqual(stop.call_args_list, [mock.call(sender), mock.call(harness)])
                for call in launch.call_args_list:
                    self.assertTrue(call.kwargs["start_new_session"])
                    self.assertTrue(call.kwargs["stdout"].closed)
                self.assertEqual(analyze.call_count, 1 if analyzer_error else raw_count + 1)
                for track, call in enumerate(analyze.call_args_list[1:]):
                    command = call.args[0]
                    self.assertEqual(Path(command[1]).name, "measure_mixer_trace.py")
                    self.assertTrue(command[command.index("--tracefile") + 1].endswith(f".mix{track}.csv"))
                    self.assertEqual(command[command.index("--max-offset-ms") + 1], "2")
                    self.assertIn("--current-generation-window", command)

    def test_cleanup_failure_does_not_skip_other_children_or_open_logs(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(io.StringIO()) as errors:
            with self.assertRaisesRegex(RuntimeError, "Incomplete child cleanup") as raised:
                self.run_mock_case(Path(temporary), analyzer_error=RuntimeError("analyzer fixture failure"),
                                   cleanup_effect=[OSError("cleanup fixture failure"), None])
            self.assertIn("cleanup fixture failure", errors.getvalue())
            self.assertEqual(str(raised.exception.__context__), "analyzer fixture failure")

    def test_cleanup_failure_cannot_report_pass(self):
        with tempfile.TemporaryDirectory() as temporary, contextlib.redirect_stderr(io.StringIO()):
            with self.assertRaisesRegex(RuntimeError, "Incomplete child cleanup"):
                self.run_mock_case(Path(temporary), cleanup_effect=[OSError("cleanup fixture failure"), None])

    def test_mute_override_requires_expected_analyzer_rejection(self):
        for code, passed in ((0, False), (1, False), (2, True), (3, False)):
            with self.subTest(analyzer_exit=code), tempfile.TemporaryDirectory() as temporary:
                result = self.run_mock_case(Path(temporary), case="mute", analyzer_code=code,
                                           analyzer_report=mute_report())
                self.assertEqual(result["passed"], passed)

    def test_raw_failure_cannot_be_hidden_by_encoded_pass(self):
        for raw_codes in ((1, 0), (0, 3), (1, 1)):
            with self.subTest(raw_codes=raw_codes), tempfile.TemporaryDirectory() as temporary:
                result = self.run_mock_case(Path(temporary), raw_codes=raw_codes)
                self.assertEqual(result["analyzer_exit"], 0)
                self.assertFalse(result["passed"])
                self.assertEqual([item["exit"] for item in result["raw_timing_checks"]], list(raw_codes))

    def test_raw_failure_cannot_be_hidden_by_successful_mute_override(self):
        for case in ("mute", "rapid-mute"):
            with self.subTest(case=case), tempfile.TemporaryDirectory() as temporary:
                result = self.run_mock_case(Path(temporary), case=case, analyzer_code=2,
                                           analyzer_report=mute_report(), raw_codes=(1,))
                self.assertTrue(result["mute_fixture"]["passed"])
                self.assertFalse(result["passed"])
                self.assertEqual(result["raw_timing_checks"], [{"track": 0, "exit": 1, "passed": False}])

    def test_successful_raw_gates_do_not_override_encoded_failure(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.run_mock_case(Path(temporary), analyzer_code=3)
            self.assertTrue(all(item["passed"] for item in result["raw_timing_checks"]))
            self.assertFalse(result["passed"])


class RunnerCliTests(unittest.TestCase):
    def invoke(self, directory, *arguments):
        # A real Python process exercises argparse and the __main__ entry point.
        # Every tested option exits before runner setup or media child creation.
        return subprocess.run([sys.executable, "-B", str(MODULE_PATH), *arguments],
                              cwd=directory, capture_output=True, text=True, timeout=10)

    def test_help_runs_without_required_paths_or_media_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            result = self.invoke(temporary, "--help")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertIn("--output-dir", result.stdout)
            self.assertIn("--case", result.stdout)
            self.assertEqual(list(Path(temporary).iterdir()), [])

    def test_invalid_arguments_never_create_output_directory(self):
        for invalid in (("--case", "unknown-case"), ("--cycles", "0"), ("--unsupported-option",)):
            with self.subTest(arguments=invalid), tempfile.TemporaryDirectory() as temporary:
                directory = Path(temporary)
                output = directory / "must-not-exist"
                result = self.invoke(temporary, "--build-dir", str(directory / "no-build"),
                                     "--obs-plugins", str(directory / "no-plugins"),
                                     "--obs-data", str(directory / "no-data"),
                                     "--output-dir", str(output), *invalid)
                self.assertEqual(result.returncode, 2, result.stderr)
                self.assertIn("error:", result.stderr)
                self.assertFalse(output.exists())
                self.assertEqual(list(directory.iterdir()), [])

    def test_missing_required_arguments_never_create_output_directory(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "must-not-exist"
            result = self.invoke(temporary, "--output-dir", str(output))
            self.assertEqual(result.returncode, 2, result.stderr)
            self.assertIn("required", result.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
