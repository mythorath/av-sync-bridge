#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Mocked runner safety/lifecycle tests; never start OBS, SSH, or media tools."""
import contextlib
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

TOOLS = Path(__file__).resolve().parents[1] / "tools"
sys.path.insert(0, str(TOOLS))
try:
    SPEC = importlib.util.spec_from_file_location("run_network_recording", TOOLS / "run_network_recording.py")
    runner = importlib.util.module_from_spec(SPEC)
    sys.modules[SPEC.name] = runner
    SPEC.loader.exec_module(runner)
finally:
    sys.path.pop(0)


class KeptBytes(io.BytesIO):
    closed_by_runner = False

    def close(self):
        self.closed_by_runner = True


class NetworkRunnerTests(unittest.TestCase):
    def args(self, case="restart"):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        root = Path(directory.name)
        args = types.SimpleNamespace(run_loopback=True, case=case, clock_port=29001,
            rtp_port=29002, rtcp_port=29003, output_dir=root / "new-output",
            receiver_helper=root / "helper.py")
        for name in ("network_build_dir", "obs_build_dir", "obs_plugins", "obs_data"):
            path = root / name
            path.mkdir()
            setattr(args, name, path)
        (args.obs_build_dir / "plugins/obs").mkdir(parents=True)
        for path in (args.network_build_dir / "avsync-network-receiver",
                     args.network_build_dir / "avsync-network-fixture-sender",
                     args.obs_build_dir / "avsync-obs-smoke",
                     args.obs_build_dir / "plugins/obs/avsync-obs.so", args.receiver_helper):
            path.write_bytes(b"fixture, never executed")
        return args

    def trial(self, case="restart"):
        args = self.args(case)
        args.output_dir.mkdir(mode=0o700)
        return runner.Trial(args)

    @staticmethod
    def child(path, code=None):
        process = mock.Mock()
        process.pid = 424242
        process.poll.return_value = code
        process.returncode = code
        process.wait.return_value = 0
        process.stdin = mock.Mock()
        process.stdin.fileno.return_value = 77
        reader = mock.Mock()
        reader.is_alive.return_value = False
        return runner.Child(process, path, reader=reader)

    @staticmethod
    def sender_summary(count=6):
        return {"schema": 1, "status": "control_stopped", "generated_audio": True,
                "reason": "none", "clock_usable": True, "original_anchors_transmitted": True,
                "media_verified": False, "generated_markers": count, "generated_packets": 120,
                "generated_frames": 57600, "mapped_packets": 120, "rtp_packets_at_output": 120,
                "anchored_rtp_packets": 120, "sender_reports": 3, "anchor_transport_errors": 0,
                "queue_overflows": 0, "clock_loss_count": 0}

    def test_validation_requires_explicit_linux_loopback_before_any_spawn(self):
        args = self.args()
        with mock.patch.object(runner.subprocess, "Popen") as spawn:
            for platform, enabled in (("win32", True), ("darwin", True), ("linux", False)):
                args.run_loopback = enabled
                with self.subTest(platform=platform), mock.patch.object(runner.sys, "platform", platform):
                    with self.assertRaisesRegex(runner.TrialFailure, "explicit_linux_loopback_required"):
                        runner.validate_args(args)
            args.run_loopback = True
            with mock.patch.object(runner.sys, "platform", "linux"):
                runner.validate_args(args)
            spawn.assert_not_called()
        self.assertFalse(args.output_dir.exists())

    def test_ports_must_be_distinct_unprivileged_exact_integers(self):
        for value in (0, 1023, 65536, True, 29001.0, "29001", 29002):
            args = self.args()
            args.clock_port = value
            with self.subTest(port=value), mock.patch.object(runner.sys, "platform", "linux"):
                with self.assertRaisesRegex(runner.TrialFailure, "invalid_loopback_ports"):
                    runner.validate_args(args)

    def test_existing_output_missing_parent_and_missing_inputs_refused(self):
        for failure in ("existing_directory", "existing_file", "parent", "binary", "directory"):
            args = self.args()
            if failure == "existing_directory":
                args.output_dir.mkdir()
            elif failure == "existing_file":
                args.output_dir.write_text("preserve", encoding="ascii")
            elif failure == "parent":
                args.output_dir = args.output_dir / "absent-child"
            elif failure == "binary":
                args.receiver_helper = args.output_dir / "absent-helper"
            else:
                args.obs_plugins = args.receiver_helper
            with self.subTest(failure=failure), mock.patch.object(runner.sys, "platform", "linux"):
                with self.assertRaises((runner.TrialFailure, OSError)):
                    runner.validate_args(args)
        args = self.args()
        with mock.patch.object(runner.sys, "platform", "linux"), \
                mock.patch.object(Path, "is_symlink", return_value=True):
            with self.assertRaisesRegex(runner.TrialFailure, "output_directory_exists"):
                runner.validate_args(args)

    def test_receiver_arguments_pin_both_interfaces_to_loopback_and_require_control(self):
        args = self.args()
        argv = runner.receiver_config(args, args.output_dir / "desktop.ipc")["receiver_argv"]
        for flag in ("--bind", "--peer"):
            self.assertEqual(argv[argv.index(flag) + 1], "127.0.0.1")
        self.assertIn("--replace-desktop-ipc", argv)
        self.assertIn("--control-stdin", argv)
        self.assertEqual(argv[argv.index("--expect-sender-session") + 1], "{sender_session}")
        self.assertEqual(argv[argv.index("--seconds") + 1], "{seconds}")

    def test_private_new_directories_requested_and_old_output_never_reused(self):
        args = self.args()
        modes = []
        original = Path.mkdir

        def mkdir(path, *values, **keywords):
            modes.append((path, keywords.get("mode")))
            return original(path, *values, **keywords)

        with mock.patch.object(Path, "mkdir", mkdir), \
                mock.patch.object(runner.Trial, "record"), mock.patch.object(runner.Trial, "analyze", return_value={"passed": True}), \
                mock.patch.object(runner.Trial, "cleanup", return_value=[]):
            self.assertTrue(runner.run(args)["passed"])
        self.assertIn((args.output_dir, 0o700), modes)
        self.assertIn((args.output_dir / "state", 0o700), modes)
        with mock.patch.object(runner, "Trial") as construct, self.assertRaises(FileExistsError):
            runner.run(args)
        construct.assert_not_called()

    def test_launch_owns_child_before_control_failure_and_uses_new_session(self):
        trial = self.trial()
        process = self.child(trial.root / "unused").process
        process.stdout = io.BytesIO(b"bounded output\n")
        with mock.patch.object(runner.subprocess, "Popen", return_value=process) as spawn, \
                mock.patch.object(runner.os, "set_blocking") as blocking:
            child = trial.launch(["never-executed", "literal argument"], "launch.log", control=True)
        child.reader.join(timeout=1)
        self.assertIn(child, trial.children)
        self.assertIn(child, trial.controlled)
        blocking.assert_called_once_with(77, False)
        self.assertEqual(child.path.read_bytes(), b"bounded output\n")
        keywords = spawn.call_args.kwargs
        self.assertTrue(keywords["start_new_session"])
        self.assertFalse(keywords["shell"])
        self.assertEqual(keywords["stdin"], subprocess.PIPE)
        self.assertEqual(keywords["stderr"], subprocess.STDOUT)
        self.assertEqual(keywords["bufsize"], 0)
        process.stdout = io.BytesIO(b"")
        with mock.patch.object(runner.subprocess, "Popen", return_value=process), \
                mock.patch.object(runner.os, "set_blocking", side_effect=OSError("private")):
            with self.assertRaises(OSError):
                trial.launch(["never-executed"], "failed-control.log", control=True)
        self.assertEqual(len(trial.children), 2)
        trial.children[-1].reader.join(timeout=1)

    def test_drain_caps_disk_bytes_and_marks_read_failures(self):
        output = KeptBytes()
        source = mock.Mock()
        source.read.side_effect = [b"1234", b"5678", b""]
        errors = []
        with mock.patch.object(runner, "MAX_LOG", 5):
            runner.drain_output(source, output, errors)
        self.assertEqual(output.getvalue(), b"1234")
        self.assertEqual(errors, ["child_output_limit"])
        self.assertTrue(output.closed_by_runner)
        source.close.assert_called_once()
        errors = []
        source.read.side_effect = OSError("private failure")
        runner.drain_output(source, KeptBytes(), errors)
        self.assertEqual(errors, ["child_output_read_error"])
        source.read.side_effect = [b"1234", b""]
        output = mock.Mock()
        output.write.return_value = 3
        errors = []
        runner.drain_output(source, output, errors)
        self.assertEqual(errors, ["child_output_read_error"])

    def test_partial_json_and_utf8_are_not_promoted_to_complete_records(self):
        trial = self.trial()
        path = trial.root / "partial.log"
        path.write_bytes(b'AVSYNC_CONTROL {"event":"ready"}\nAVSYNC_CONTROL {"x":"\xc3')
        self.assertEqual(runner.complete_lines(path), ['AVSYNC_CONTROL {"event":"ready"}'])
        with self.assertRaisesRegex(runner.TrialFailure, "child_output_encoding"):
            runner.read_log(path)
        path.write_bytes(b"completed bad encoding \xff\n")
        with self.assertRaisesRegex(runner.TrialFailure, "child_output_encoding"):
            runner.complete_lines(path)
        with mock.patch.object(runner, "MAX_LOG", 4), self.assertRaises(runner.TrialFailure):
            runner.read_bytes(path)

    def test_wait_record_requires_exactly_one_complete_bounded_object(self):
        trial = self.trial()
        trial.pump = mock.Mock()
        child = self.child(trial.root / "records.log")
        predicate = lambda value: value.get("event") == "ready"
        valid = 'FIXTURE {"event":"ready"}\n'
        child.path.write_text(valid + 'FIXTURE {"event":', encoding="ascii")
        self.assertEqual(trial.wait_record(child, "FIXTURE ", predicate, .2), {"event": "ready"})
        for text in (valid * 2, 'FIXTURE {"event":"ready","event":"ready"}\n',
                     'FIXTURE {"event":"ready","value":NaN}\n', "FIXTURE []\n",
                     "FIXTURE " + "x" * 2049 + "\n"):
            child.path.write_text(text, encoding="ascii")
            with self.subTest(text=text[:80]), self.assertRaises(runner.TrialFailure):
                trial.wait_record(child, "FIXTURE ", predicate, .2)
        child.path.write_text(valid, encoding="ascii")
        child.process.poll.return_value = 0
        with self.assertRaisesRegex(runner.TrialFailure, "child_exited_before_record"):
            trial.wait_record(child, "FIXTURE ", predicate, .2)

    def test_control_writes_must_be_complete_and_nonblocking(self):
        child = self.child(Path("unused"))
        for result in (0, 1):
            with mock.patch.object(runner.os, "write", return_value=result), \
                    self.assertRaisesRegex(runner.TrialFailure, "short_control_write"):
                runner.Trial.send(child, b"STOP\n")
        with mock.patch.object(runner.os, "write", side_effect=BlockingIOError("private")), \
                self.assertRaisesRegex(runner.TrialFailure, "control_write_failed"):
            runner.Trial.send(child, b"STOP\n")

    def test_owned_cleanup_signals_only_own_session_with_finite_escalation(self):
        child = self.child(Path("unused"))
        child.process.wait.side_effect = [subprocess.TimeoutExpired("fixture", 1), 0]
        with mock.patch.object(runner.os, "getpgid", return_value=child.process.pid, create=True), \
                mock.patch.object(runner.os, "killpg", create=True) as kill, \
                mock.patch.object(runner.signal, "SIGCONT", 18, create=True), \
                mock.patch.object(runner.signal, "SIGTERM", 15, create=True), \
                mock.patch.object(runner.signal, "SIGKILL", 9, create=True):
            runner.stop_owned(child)
        self.assertEqual(kill.call_args_list, [mock.call(424242, 18), mock.call(424242, 15), mock.call(424242, 9)])
        self.assertEqual(child.process.wait.call_args_list, [mock.call(timeout=1), mock.call(timeout=1)])
        child.process.stdin.close.assert_called_once()
        child.reader.join.assert_called_once_with(timeout=.5)
        child = self.child(Path("unused"))
        with mock.patch.object(runner.os, "getpgid", return_value=7, create=True), \
                mock.patch.object(runner.os, "killpg", create=True) as kill, \
                self.assertRaisesRegex(runner.TrialFailure, "owned_session_mismatch"):
            runner.stop_owned(child)
        kill.assert_not_called()

    def test_dead_owned_child_is_not_signaled_and_reader_must_finish(self):
        child = self.child(Path("unused"), code=0)
        with mock.patch.object(runner.os, "killpg", create=True) as kill:
            runner.stop_owned(child)
            kill.assert_not_called()
            child.reader.is_alive.return_value = True
            with self.assertRaisesRegex(runner.TrialFailure, "child_output_not_closed"):
                runner.stop_owned(child)

    def test_pump_deadline_output_and_process_failures_are_terminal(self):
        trial = self.trial()
        trial.deadline = 10
        child = self.child(trial.root / "child.log")
        trial.children = [child]
        trial.controlled[child] = 0
        trial.send = mock.Mock()
        with mock.patch.object(runner.time, "monotonic", return_value=1):
            trial.pump()
        trial.send.assert_called_once_with(child, b"AVSYNC_KEEPALIVE\n")
        with mock.patch.object(runner.time, "monotonic", return_value=10), \
                self.assertRaisesRegex(runner.TrialFailure, "whole_trial_deadline"):
            trial.pump()
        child.errors.append("child_output_limit")
        with mock.patch.object(runner.time, "monotonic", return_value=1), \
                self.assertRaisesRegex(runner.TrialFailure, "child_output_failed"):
            trial.pump()
        child.errors.clear()
        child.process.poll.return_value = 0
        with mock.patch.object(runner.time, "monotonic", return_value=1), \
                self.assertRaisesRegex(runner.TrialFailure, "controlled_child_exited"):
            trial.pump()
        trial.controlled.clear()
        trial.display = child
        with mock.patch.object(runner.time, "monotonic", return_value=1), \
                self.assertRaisesRegex(runner.TrialFailure, "recorder_exited_early"):
            trial.pump()

    def test_generated_sender_summary_strictly_qualifies_only_exact_types_and_counters(self):
        trial = self.trial()
        child = self.child(trial.root / "sender.log")
        valid = self.sender_summary()
        valid["private_note"] = "DO_NOT_PUBLISH_PRIVATE_VALUE"
        child.path.write_text(json.dumps(valid) + "\n", encoding="ascii")
        report = trial.native_sender(child, 6)
        self.assertEqual(report["generated_markers"], 6)
        self.assertNotIn("DO_NOT_PUBLISH", json.dumps(report))
        variants = [("schema", True), ("status", "error"), ("generated_audio", 1), ("reason", "private"),
                    ("clock_usable", 1), ("original_anchors_transmitted", 1), ("media_verified", True),
                    ("generated_markers", True), ("generated_markers", 5), ("mapped_packets", 119),
                    ("rtp_packets_at_output", 119), ("generated_frames", 57601)]
        for key in ("generated_packets", "generated_frames", "mapped_packets", "rtp_packets_at_output",
                    "anchored_rtp_packets", "sender_reports"):
            variants += [(key, value) for value in (0, True, 1.0, 1 << 64)]
        for key in ("anchor_transport_errors", "queue_overflows", "clock_loss_count"):
            variants += [(key, value) for value in (1, False, 0.0)]
        for key, value in variants:
            child.path.write_text(json.dumps(dict(valid, **{key: value})) + "\n", encoding="ascii")
            with self.subTest(key=key, value=value), self.assertRaises(runner.TrialFailure):
                trial.native_sender(child, 6)
        for text in ("", (json.dumps(valid) + "\n") * 2, json.dumps(valid)):
            child.path.write_text(text, encoding="ascii")
            with self.assertRaisesRegex(runner.TrialFailure, "native_sender_summary_missing"):
                trial.native_sender(child, 6)

    def test_manifest_requires_exact_runtime_session_epoch_and_generation(self):
        trial = self.trial()
        child = self.child(trial.root / "sender.log")
        child.path.write_text("fixture", encoding="ascii")
        good = {"header": {"sender_session": "12", "clock_epoch": "34", "generation": "1"}, "events": []}
        with mock.patch.object(runner, "parse_manifest", return_value=good):
            self.assertEqual(trial.manifest(child, 1, 3, "12", "34"), good)
        for key in ("sender_session", "clock_epoch", "generation"):
            value = {"header": dict(good["header"], **{key: "999"})}
            with mock.patch.object(runner, "parse_manifest", return_value=value), \
                    self.assertRaisesRegex(runner.TrialFailure, "fixture_identity_mismatch"):
                trial.manifest(child, 1, 3, "12", "34")
        with mock.patch.object(runner, "parse_manifest", side_effect=ValueError("private")), \
                self.assertRaisesRegex(runner.TrialFailure, "invalid_fixture_manifest"):
            trial.manifest(child, 1, 3, "12", "34")

    def lifecycle(self, fail_retirement=False, fail_launch=False):
        trial = self.trial()
        trial.config = trial.root / "receiver.json"
        trial.pump = mock.Mock()
        trial.finish_output = mock.Mock()
        trial.manifest = mock.Mock()
        trial.native_sender = mock.Mock(return_value={"fixture": True})
        trial.native_receiver = mock.Mock(return_value={"fixture": True})
        events = []

        def launch(argv, name, **keywords):
            events.append(("launch", name))
            if fail_launch:
                raise OSError("private launch detail")
            child = self.child(trial.root / name, code=0)
            child.process.returncode = 0
            trial.children.append(child)
            if keywords.get("control"):
                trial.controlled[child] = 0
            return child

        def wait_record(child, prefix, match, seconds):
            role = int(child.path.stem.rsplit("-", 1)[1])
            if prefix == "AVSYNC_CONTROL ":
                return {"schema": 1, "event": "receiver_ready" if child.path.name.startswith("receiver") else "sender_started",
                        "sender_session": str(122 + role), "clock_epoch": str(5000 + role)}
            return {"type": "marker", "event": 3 if role == 1 else 6, "capture_ns": "1000000000"}

        def retire(run_id, session):
            events.append(("retire", run_id, session))
            if fail_retirement:
                raise runner.TrialFailure("retirement_not_verified")
            return {"verified": True, "proof": "fenced_empty_cgroup"}

        trial.launch, trial.wait_record, trial.retire = mock.Mock(side_effect=launch), mock.Mock(side_effect=wait_record), mock.Mock(side_effect=retire)
        trial.send = mock.Mock()
        return trial, events

    def test_restart_retires_before_successor_and_uses_fresh_session_and_clock(self):
        trial, events = self.lifecycle()
        with mock.patch.object(runner.secrets, "token_hex", side_effect=["a" * 32, "b" * 32]), \
                mock.patch.object(runner.secrets, "randbits", side_effect=[123, 124]), \
                mock.patch.object(runner.time, "monotonic_ns", return_value=3_200_000_000):
            trial.run_roles()
        self.assertEqual([item[:2] for item in events], [("launch", "receiver-1.log"), ("launch", "sender-1.log"),
            ("retire", "a" * 32), ("launch", "receiver-2.log"), ("launch", "sender-2.log"), ("retire", "b" * 32)])
        self.assertEqual(trial.pending, [])
        self.assertEqual(trial.controlled, {})
        self.assertEqual([report["interrupted"] for report in trial.reports], [True, False])
        self.assertIsNone(trial.reports[0]["receiver_native"])
        trial.children[0].process.kill.assert_called_once()
        for call in trial.launch.call_args_list:
            if "sender" in call.args[1]:
                argv = call.args[0]
                self.assertIn("--generated-audio", argv)
                self.assertEqual(argv[argv.index("--host") + 1], "127.0.0.1")

    def test_failed_retirement_prevents_any_successor_replacement(self):
        trial, events = self.lifecycle(fail_retirement=True)
        with mock.patch.object(runner.secrets, "token_hex", return_value="a" * 32), \
                mock.patch.object(runner.secrets, "randbits", return_value=123), \
                mock.patch.object(runner.time, "monotonic_ns", return_value=3_200_000_000):
            with self.assertRaisesRegex(runner.TrialFailure, "retirement_not_verified"):
                trial.run_roles()
        self.assertEqual(len(trial.children), 2)
        self.assertEqual(trial.pending, [("a" * 32, "123")])
        self.assertFalse(any(item == ("launch", "receiver-2.log") for item in events))

    def test_failed_receiver_launch_still_leaves_exact_pending_fence(self):
        trial, _ = self.lifecycle(fail_launch=True)
        with mock.patch.object(runner.secrets, "token_hex", return_value="a" * 32), \
                mock.patch.object(runner.secrets, "randbits", return_value=123):
            with self.assertRaises(OSError):
                trial.run_roles()
        self.assertEqual(trial.children, [])
        self.assertEqual(trial.pending, [("a" * 32, "123")])
        self.assertEqual(trial.cleanup(), [])
        trial.retire.assert_called_once_with("a" * 32, "123")
        self.assertEqual(trial.pending, [])

    def test_cleanup_attempts_every_child_and_pending_fence_despite_errors(self):
        trial = self.trial()
        children = [self.child(trial.root / f"child-{n}.log") for n in range(3)]
        trial.children = children
        trial.controlled = {child: 0 for child in children}
        trial.pending = [("a" * 32, "123"), ("b" * 32, "124")]
        order = []

        def stop(child):
            self.assertEqual(trial.controlled, {})
            order.append(("stop", child.path.name))
            if child is children[2]:
                raise OSError("private local cleanup detail")

        def retire(run_id, session):
            order.append(("retire", run_id))
            if session == "123":
                raise runner.TrialFailure("retirement_not_verified")

        trial.retire = mock.Mock(side_effect=retire)
        with mock.patch.object(runner, "stop_owned", side_effect=stop):
            errors = trial.cleanup()
        self.assertEqual(order, [("stop", "child-2.log"), ("stop", "child-1.log"), ("stop", "child-0.log"),
                                 ("retire", "a" * 32), ("retire", "b" * 32)])
        self.assertEqual(errors, ["owned_child_cleanup_failed", "retirement_cleanup_failed"])
        self.assertEqual(trial.pending, [("a" * 32, "123")])

    def test_retirement_query_is_bounded_exact_and_always_stopped(self):
        trial = self.trial()
        child = self.child(trial.root / "proof.log", code=0)
        proof = {"schema": 1, "event": "receiver_retired", "run_id": "a" * 32,
                 "sender_session": "123", "challenge": "b" * 32, "proof": "fenced_empty_cgroup"}
        trial.launch = mock.Mock(return_value=child)
        trial.finish_output = mock.Mock()
        for text, code, success in ((json.dumps(proof), 0, True), (json.dumps(proof), 7, False),
                                    (json.dumps(dict(proof, challenge="c" * 32)), 0, False)):
            child.path.write_text("AVSYNC_RETIRE " + text + "\n", encoding="ascii")
            child.process.wait.return_value = code
            with mock.patch.object(runner.secrets, "token_hex", return_value="b" * 32), \
                    mock.patch.object(runner, "stop_owned") as stop:
                if success:
                    self.assertTrue(trial.retire("a" * 32, "123")["verified"])
                else:
                    with self.assertRaises(runner.TrialFailure):
                        trial.retire("a" * 32, "123")
                stop.assert_called_once_with(child)
            child.process.wait.assert_called_with(timeout=10)
        argv = trial.launch.call_args.args[0]
        self.assertIn("retire", argv)
        self.assertEqual(argv[argv.index("--run-id") + 1], "a" * 32)
        self.assertEqual(argv[argv.index("--expect-sender-session") + 1], "123")

    def test_retirement_preserves_primary_failure_even_if_local_cleanup_fails(self):
        trial = self.trial()
        child = self.child(trial.root / "proof.log", code=0)
        trial.launch = mock.Mock(return_value=child)
        trial.finish_output = mock.Mock()
        child.path.write_text("unused", encoding="ascii")
        for failure, reason in ((7, "retirement_query_failed"),
                                (subprocess.TimeoutExpired("fixture", 10), "retirement_not_verified"),
                                (0, "retirement_local_cleanup_failed")):
            child.process.wait.side_effect = failure if isinstance(failure, Exception) else None
            child.process.wait.return_value = failure if isinstance(failure, int) else 0
            with mock.patch.object(runner, "stop_owned", side_effect=OSError("DO_NOT_PUBLISH")) as stop, \
                    mock.patch.object(runner, "parse_retirement", return_value="fenced_empty_cgroup"), \
                    self.assertRaisesRegex(runner.TrialFailure, "^" + reason + "$"):
                trial.retire("a" * 32, "123")
            stop.assert_called_once_with(child)

    def test_late_child_output_errors_cannot_disappear_during_cleanup(self):
        trial = self.trial()
        child = self.child(trial.root / "child.log", code=0)
        trial.children = [child]

        def finish(_):
            child.errors.append("child_output_limit")

        with mock.patch.object(runner, "stop_owned", side_effect=finish):
            self.assertEqual(trial.cleanup(), ["owned_child_output_failed"])

    def test_run_retains_initial_failure_and_reports_cleanup_separately_without_private_text(self):
        for first in (runner.TrialFailure("fixture_record_timeout"), OSError("DO_NOT_PUBLISH_PRIVATE_PATH")):
            args = self.args()
            trial = mock.Mock()
            trial.reports = []
            trial.record.side_effect = first
            trial.cleanup.return_value = ["retirement_cleanup_failed"]
            with mock.patch.object(runner, "Trial", return_value=trial):
                report = runner.run(args)
            self.assertFalse(report["passed"])
            self.assertEqual(report["failure"], "fixture_record_timeout" if isinstance(first, runner.TrialFailure) else "trial_failed")
            self.assertEqual(report["cleanup_failures"], ["retirement_cleanup_failed"])
            trial.cleanup.assert_called_once()
            self.assertNotIn("DO_NOT_PUBLISH", json.dumps(report))
            self.assertNotIn("DO_NOT_PUBLISH", (args.output_dir / "result.json").read_text())
            self.assertFalse(report["queued_stale_replay_verified"])

    def test_success_is_revoked_if_cleanup_itself_raises(self):
        args = self.args()
        trial = mock.Mock()
        trial.reports = []
        trial.analyze.return_value = {"passed": True}
        trial.cleanup.side_effect = RuntimeError("DO_NOT_PUBLISH")
        with mock.patch.object(runner, "Trial", return_value=trial):
            report = runner.run(args)
        self.assertFalse(report["passed"])
        self.assertEqual(report["cleanup_failures"], ["cleanup_failed"])
        self.assertNotIn("DO_NOT_PUBLISH", json.dumps(report))

    def test_report_write_failure_preserves_primary_and_cleanup_failures(self):
        args = self.args()
        trial = mock.Mock()
        trial.reports = []
        trial.record.side_effect = runner.TrialFailure("fixture_record_timeout")
        trial.cleanup.return_value = ["retirement_cleanup_failed"]
        original = Path.write_text

        def write(path, *values, **keywords):
            if path.name == "result.json":
                raise OSError("DO_NOT_PUBLISH_PRIVATE_PATH")
            return original(path, *values, **keywords)

        with mock.patch.object(runner, "Trial", return_value=trial), mock.patch.object(Path, "write_text", write):
            report = runner.run(args)
        self.assertFalse(report["passed"])
        self.assertTrue(report["report_write_failed"])
        self.assertEqual(report["failure"], "fixture_record_timeout")
        self.assertEqual(report["cleanup_failures"], ["retirement_cleanup_failed"])
        self.assertEqual((args.output_dir / "failure.txt").read_text(), "fixture_record_timeout")
        self.assertNotIn("DO_NOT_PUBLISH", json.dumps(report))

    def test_main_opt_in_refusal_and_setup_exception_have_sanitized_json(self):
        args = self.args()
        argv = ["--case", "baseline", "--clock-port", "29001", "--rtp-port", "29002", "--rtcp-port", "29003"]
        for name in ("network_build_dir", "obs_build_dir", "obs_plugins", "obs_data", "output_dir"):
            argv += ["--" + name.replace("_", "-"), str(getattr(args, name))]
        for enabled in (False, True):
            output = io.StringIO()
            with mock.patch.object(runner.sys, "platform", "linux"), \
                    mock.patch.object(runner, "run", side_effect=OSError("DO_NOT_PUBLISH_PRIVATE_PATH")) as run, \
                    mock.patch.object(runner.os, "umask") as umask, contextlib.redirect_stdout(output):
                code = runner.main(argv + (["--run-loopback", "--receiver-helper", str(args.receiver_helper)] if enabled else []))
            self.assertEqual(code, 1)
            report = json.loads(output.getvalue())
            self.assertEqual(report["failure"], "setup_failed" if enabled else "explicit_linux_loopback_required")
            self.assertNotIn("DO_NOT_PUBLISH", output.getvalue())
            if enabled:
                umask.assert_called_once_with(0o077)
            else:
                run.assert_not_called()
                umask.assert_not_called()


if __name__ == "__main__":
    unittest.main()
