# SPDX-License-Identifier: GPL-2.0-or-later
"""Software-only child-process fixtures: no SSH, devices, audio, or OBS."""

from __future__ import annotations

import argparse
import importlib.util
import json
import os
from pathlib import Path
import queue
import subprocess
import sys
import tempfile
import threading
import time
import unittest


MODULE_PATH = Path(__file__).resolve().parents[1] / "tools" / "process_pair.py"
SPEC = importlib.util.spec_from_file_location("avsync_process_pair", MODULE_PATH)
pair = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pair
SPEC.loader.exec_module(pair)


def fake_child() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fake-child", choices=("receiver", "sender"), required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--expect-sender-session")
    parser.add_argument("--sender-session")
    parser.add_argument("--clock-epoch")
    parser.add_argument("--seconds")
    parser.add_argument("--control-stdin", action="store_true")
    parser.add_argument("--loopback", action="store_true")
    args = parser.parse_args()
    role, mode = args.fake_child, args.mode
    counter_file = args.state / (role + ".count")
    count = int(counter_file.read_text()) + 1 if counter_file.exists() else 1
    counter_file.write_text(str(count))
    session = args.expect_sender_session if role == "receiver" else args.sender_session
    clock = ("1000" if mode == "reused_provider" else str(1000 + count)) if role == "receiver" else args.clock_epoch
    log = args.state / (role + ".log")

    def record(event: str) -> None:
        with log.open("a", encoding="utf-8") as handle:
            handle.write(json.dumps({"event": event, "pid": os.getpid(), "session": session,
                                     "clock": clock, "count": count}) + "\n")

    record("start")
    message = {"schema": 1, "event": "receiver_ready" if role == "receiver" else "sender_started",
               "clock_epoch": clock, "sender_session": session}
    if mode == "stale_ack" and role == "sender":
        message["clock_epoch"] = str(int(clock) + 1)
    if mode == "foreign_session" and role == "receiver":
        message["sender_session"] = "1" if session != "1" else "2"
    line = b"AVSYNC_CONTROL " + json.dumps(message).encode("ascii") + b"\n"
    suppress = (mode == "no_ready" and role == "receiver") or (mode in ("no_ack", "stderr_ack") and role == "sender")
    if mode == "long_line" and role == "receiver":
        sys.stdout.buffer.write(b"x" * (pair.MAX_LINE + 20) + b"\n")
        sys.stdout.buffer.flush()
    if mode == "noise" and role == "receiver":
        # More than pipe capacity, without retaining unbounded diagnostic logs.
        sys.stdout.buffer.write((b"ignored diagnostic\n" * 10000))
        sys.stderr.buffer.write(b"ignored stderr\n" * 10000)
        sys.stderr.buffer.flush()
    if not suppress:
        sys.stdout.buffer.write(line)
        if mode == "duplicate_ready" and role == "receiver":
            sys.stdout.buffer.write(line)
        if mode == "control_flood" and role == "receiver":
            sys.stdout.buffer.write(line * 1000)
        sys.stdout.buffer.flush()
    if mode == "stderr_ack" and role == "sender":
        sys.stderr.buffer.write(line)
        sys.stderr.buffer.flush()
    if mode in ("stdout_eof", "eof_bad_stop") and role == "sender":
        os.close(sys.stdout.fileno())

    commands: queue.Queue[bytes] = queue.Queue()

    def read_input() -> None:
        while True:
            command = sys.stdin.buffer.readline()
            commands.put(command)
            if not command:
                return

    threading.Thread(target=read_input, daemon=True).start()
    lease = time.monotonic() + 5
    while time.monotonic() < lease:
        try:
            command = commands.get(timeout=.01)
        except queue.Empty:
            continue
        if command == pair.KEEPALIVE:
            record("keepalive")
            lease = time.monotonic() + 5
        elif command == pair.STOP:
            if mode == "ignore_stop":
                continue
            record("stop")
            return 7 if mode in ("bad_stop", "eof_bad_stop") else 0
        elif not command:
            record("eof")
            return 1
        else:
            record("invalid")
            return 1
    record("lease_expired")
    return 1


def raw_config(state: Path, mode: str = "healthy", **changes) -> dict:
    common = [sys.executable, str(Path(__file__).resolve()), "--mode", mode, "--state", str(state)]
    result = {
        "receiver_argv": common + ["--fake-child", "receiver", "--expect-sender-session",
                                   "{sender_session}", "--seconds", "{seconds}", "--control-stdin"],
        "sender_argv": common + ["--fake-child", "sender", "--sender-session", "{sender_session}",
                                 "--clock-epoch", "{clock_epoch}", "--seconds", "{seconds}",
                                 "--control-stdin", "--loopback"],
        # Allow actual interpreter/process startup and cooperative scheduling.
        # Process-count semantics below use an explicit acknowledged-pair fault
        # barrier, not assumptions about how many milliseconds Windows needs.
        "total_seconds": 3.5,
        "ready_timeout": 1.5,
        "ack_timeout": 1.5,
        "stop_grace": .6,
        "max_attempts": 3,
    }
    return {**result, **changes}


def events(state: Path, role: str) -> list[dict]:
    path = state / (role + ".log")
    return [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []


class ProtocolTests(unittest.TestCase):
    def test_uint64_stays_decimal_string(self):
        message = {"schema": 1, "event": "receiver_ready", "clock_epoch": str((1 << 64) - 1),
                   "sender_session": "9007199254740993"}
        self.assertEqual(pair.parse_control(pair.PREFIX + json.dumps(message).encode()), message)

    def test_schema_and_identity_rejections(self):
        base = {"schema": 1, "event": "receiver_ready", "clock_epoch": "5", "sender_session": "7"}
        variants = [{**base, "clock_epoch": value} for value in (0, 5, "0", "01", "-1", " 5", "1.0", str(1 << 64))]
        variants += [{**base, "schema": True}, {**base, "schema": 2}, {**base, "event": "pcm_healthy"},
                     {**base, "extra": "untrusted"}, [base], None]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(pair.PairError):
                pair.parse_control(pair.PREFIX + json.dumps(value).encode())

    def test_duplicate_keys_bad_encoding_and_size(self):
        for line in (pair.PREFIX + b'{"schema":1,"schema":1}', pair.PREFIX + b"\xff",
                     pair.PREFIX + b"x" * 2000, b"AVSYNC_CONTROLX {}", pair.PREFIX + b"[[[["):
            with self.subTest(line=line[:50]), self.assertRaises(pair.PairError):
                pair.parse_control(line)

    def test_command_replacement_is_exact_no_shell(self):
        cfg = pair.Config.from_dict(raw_config(Path("test data")))
        argv = cfg.argv("sender", "123", "456", 5)
        self.assertIn("test data", argv)
        self.assertEqual(argv[argv.index("--sender-session") + 1], "123")
        self.assertEqual(argv[argv.index("--clock-epoch") + 1], "456")

    def test_invalid_configs(self):
        base = raw_config(Path("fixture"))
        variants = [{**base, "max_attempts": x} for x in (0, 4, True)]
        variants += [{**base, "total_seconds": x} for x in (0, 181, float("nan"), float("inf"), True)]
        variants += [{**base, "stop_grace": 0}, {**base, "extra": 1},
                     {**base, "receiver_argv": "arbitrary shell"}]
        for flag in ("--loopback", "--control-stdin", "--sender-session", "--clock-epoch"):
            argv = list(base["sender_argv"])
            argv.remove(flag)
            variants.append({**base, "sender_argv": argv})
        variants.append({**base, "receiver_argv": base["receiver_argv"] + ["--x={sender_session}"]})
        variants.append({**base, "receiver_argv": base["receiver_argv"] + ["{clock_epoch}"]})
        variants.append({**base, "receiver_argv": base["receiver_argv"] + ["--control-stdin"]})
        for value in variants:
            with self.subTest(value=value), self.assertRaises(pair.PairError):
                pair.Config.from_dict(value)

    def test_duplicate_lock_and_reacquire_preserves_file(self):
        with tempfile.TemporaryDirectory() as temp:
            lock_path = Path(temp) / "pair.lock"
            with pair.InstanceLock(lock_path):
                inode = lock_path.stat().st_ino
                with self.assertRaises(pair.PairError):
                    with pair.InstanceLock(lock_path):
                        self.fail("second lock acquired")
            with pair.InstanceLock(lock_path):
                self.assertEqual(lock_path.stat().st_ino, inode)
            self.assertTrue(lock_path.exists())

    @unittest.skipIf(os.name == "nt", "POSIX ownership/mode policy")
    def test_unsafe_lock_permissions_symlink_hardlink_and_fifo(self):
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp)
            lock = state / "pair.lock"
            lock.touch(mode=0o600)
            os.chmod(lock, 0o644)
            with self.assertRaises(pair.PairError):
                with pair.InstanceLock(lock):
                    pass
            os.chmod(lock, 0o600)
            os.link(lock, state / "other-link")
            with self.assertRaises(pair.PairError):
                with pair.InstanceLock(lock):
                    pass
            alias = state / "symlink"
            alias.symlink_to(lock)
            with self.assertRaises(pair.PairError):
                with pair.InstanceLock(alias):
                    pass
            fifo = state / "fifo"
            os.mkfifo(fifo, 0o600)
            with self.assertRaises(pair.PairError):
                with pair.InstanceLock(fifo):
                    pass
            os.chmod(state, 0o755)
            with self.assertRaises(pair.PairError):
                with pair.InstanceLock(state / "new.lock"):
                    pass
            os.chmod(state, 0o700)


class ProcessTests(unittest.TestCase):
    def assertEqual(self, first, second, msg=None):
        # Include retained protocol/cleanup history in every fixture assertion.
        if msg is None and hasattr(self, "last_result"):
            msg = self.last_result
        return super().assertEqual(first, second, msg)

    def run_fixture(self, mode="healthy", **changes):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        state = Path(directory.name)
        config = pair.Config.from_dict(raw_config(state, mode, **changes))
        class FaultBarrierSupervisor(pair.Supervisor):
            """Inject a real child crash only after both acknowledgments.

            Waiting for the exact Popen handle makes the known-dead recovery
            test independent of stdout-EOF versus Windows exit publication.
            The ambiguous EOF/nonzero-stop branch is tested separately below.
            """
            def __init__(self, *args):
                super().__init__(*args)
                self.injected_attempts = set()

            def _poll(self, expected=None, event=None):
                attempt = self.result.attempts
                inject = ((mode in ("receiver_dies_once", "sender_dies_once", "reused_provider") and attempt == 1)
                          or mode == "always_dies")
                if expected is None and inject and attempt not in self.injected_attempts:
                    if self.result.acknowledged_pairs < attempt:
                        raise AssertionError("crash injected before complete pair agreement")
                    role = "sender" if mode == "sender_dies_once" else "receiver"
                    target = next(child for child in self.children if child.role == role)
                    target.process.kill()
                    target.process.wait(timeout=3)
                    self.injected_attempts.add(attempt)
                return super()._poll(expected, event)

        supervisor = FaultBarrierSupervisor(config, state / "pair.lock")
        result = supervisor.run()
        self.last_result = result
        self.assertEqual(supervisor.children, [])
        self.assertEqual(result.cleanup_failures, 0)
        self.assertLess(result.elapsed_seconds, config.total_seconds + .4)
        return result, state

    def test_success_is_only_process_agreement_with_cooperative_cleanup(self):
        result, state = self.run_fixture()
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.evidence, "process_agreement_only")
        self.assertEqual(result.acknowledged_pairs, 1)
        self.assertEqual(result.forced_local_kills, 0)
        for role in ("receiver", "sender"):
            log = events(state, role)
            self.assertEqual(log[-1]["event"], "stop")
            self.assertTrue(any(event["event"] == "keepalive" for event in log))

    def test_receiver_restart_retires_sender_and_uses_fresh_pair(self):
        result, state = self.run_fixture("receiver_dies_once", total_seconds=5)
        self.assertEqual(result.exit_code, 3)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.acknowledged_pairs, 2)
        starts = [event for event in events(state, "sender") if event["event"] == "start"]
        self.assertEqual(len(starts), 2)
        self.assertNotEqual(starts[0]["session"], starts[1]["session"])
        self.assertNotEqual(starts[0]["clock"], starts[1]["clock"])
        self.assertTrue(any(fault.reason.startswith("receiver_") for fault in result.faults))
        self.assertEqual(len([e for e in events(state, "sender") if e["event"] == "stop"]), 2)

    def test_sender_restart_retires_receiver(self):
        result, state = self.run_fixture("sender_dies_once", total_seconds=5)
        self.assertEqual(result.exit_code, 3)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(len([e for e in events(state, "receiver") if e["event"] == "stop"]), 2)
        self.assertTrue(any(fault.reason.startswith("sender_") for fault in result.faults))

    def test_stale_ack_never_counts_as_agreed_pair(self):
        result, _ = self.run_fixture("stale_ack", max_attempts=1)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.acknowledged_pairs, 0)
        self.assertEqual(result.faults[0].reason, "clock_epoch_mismatch")

    def test_foreign_sender_token_rejected_before_sender_launch(self):
        result, state = self.run_fixture("foreign_session", max_attempts=1)
        self.assertEqual(result.faults[0].reason, "sender_session_mismatch")
        self.assertFalse((state / "sender.count").exists())

    def test_reused_provider_epoch_rejected(self):
        result, state = self.run_fixture("reused_provider", total_seconds=5, max_attempts=2)
        self.assertEqual(result.exit_code, 1)
        self.assertTrue(any(f.reason == "reused_provider_epoch" for f in result.faults))
        self.assertEqual((state / "sender.count").read_text(), "1")

    def test_duplicate_ready_is_not_ignored(self):
        result, _ = self.run_fixture("duplicate_ready", max_attempts=1)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.faults[0].reason, "unexpected_control_event")

    def test_missing_ready_and_ack_have_bounded_waits(self):
        for mode, expected in (("no_ready", "receiver_ready_timeout"), ("no_ack", "sender_started_timeout"),
                               ("stderr_ack", "sender_started_timeout")):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, max_attempts=1)
                self.assertEqual(result.faults[0].reason, expected)
                self.assertEqual(result.acknowledged_pairs, 0)

    def test_attempt_budget_exhausted(self):
        result, _ = self.run_fixture("always_dies", total_seconds=5, max_attempts=3)
        self.assertEqual(result.exit_code, 1, result)
        self.assertEqual(result.attempts, 3, result)
        self.assertEqual(len(result.faults), 3, result)

    def test_overall_deadline_in_handshake_is_failure(self):
        result, _ = self.run_fixture("no_ready", total_seconds=1.5, ready_timeout=5)
        self.assertEqual(result.exit_code, 1)
        self.assertTrue(any(f.reason == "overall_deadline" for f in result.faults))

    def test_closed_pipe_retires_both(self):
        result, state = self.run_fixture("stdout_eof", max_attempts=1)
        self.assertEqual(result.exit_code, 1)
        self.assertTrue(any("stdout_eof" in f.reason for f in result.faults))
        self.assertEqual(events(state, "receiver")[-1]["event"], "stop")

    def test_noisy_output_is_drained_without_becoming_evidence(self):
        result, _ = self.run_fixture("noise")
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.evidence, "process_agreement_only")

    def test_output_bounds_fail_closed(self):
        for mode in ("long_line", "control_flood"):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, max_attempts=1)
                self.assertEqual(result.exit_code, 1)
                self.assertEqual(result.acknowledged_pairs, 0)

    def test_ignored_stop_kills_only_owned_children_and_fails(self):
        result, _ = self.run_fixture("ignore_stop")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.forced_local_kills, 2)
        self.assertEqual(result.faults[-1].reason, "unclean_child_stop")

    def test_nonzero_cooperative_stop_does_not_report_success(self):
        result, _ = self.run_fixture("bad_stop")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.stop_failures, 2)
        self.assertEqual(result.forced_local_kills, 0)

    def test_eof_before_nonzero_exit_is_ambiguous_and_never_restarts(self):
        result, _ = self.run_fixture("eof_bad_stop", max_attempts=3)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(any("stdout_eof" in f.reason for f in result.faults), result)
        self.assertTrue(any("nonzero_stop_exit" in f.reason for f in result.faults), result)
        self.assertEqual(result.forced_local_kills, 0)

    def test_forced_kill_after_handshake_timeout_never_restarts(self):
        # Keep the fake child's no-ready behavior but suppress cooperative stop
        # by using a known healthy child adapter with a intentionally tiny
        # receiver timeout. A unit stub makes the refusal deterministic.
        from unittest.mock import patch
        original_stop = pair.Child.send

        def ignore_stop(child, data):
            if data != pair.STOP:
                original_stop(child, data)

        with patch.object(pair.Child, "send", ignore_stop):
            result, _ = self.run_fixture("no_ready", total_seconds=2.5, max_attempts=3)
        self.assertEqual(result.attempts, 1)
        self.assertEqual(result.forced_local_kills, 1)
        self.assertEqual(result.exit_code, 1)

    def test_lock_failure_does_not_spawn(self):
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp)
            lock = state / "pair.lock"
            with pair.InstanceLock(lock):
                result = pair.Supervisor(pair.Config.from_dict(raw_config(state)), lock).run()
            self.assertEqual(result.attempts, 0)
            self.assertEqual(result.exit_code, 1)
            self.assertFalse((state / "receiver.count").exists())

    def test_child_eof_fixture_exits_without_stop(self):
        with tempfile.TemporaryDirectory() as temp:
            state = Path(temp)
            config = pair.Config.from_dict(raw_config(state))
            child = subprocess.Popen(config.argv("receiver", "123", None, 1), stdin=subprocess.PIPE,
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
            output, _ = child.communicate(timeout=3)
            self.assertIn(b"receiver_ready", output)
            self.assertEqual(child.returncode, 1)
            self.assertEqual(events(state, "receiver")[-1]["event"], "eof")


if __name__ == "__main__":
    if "--fake-child" in sys.argv:
        # Model a native process exit, without CPython waiting on the daemon
        # thread that deliberately blocks in the stdin fixture.
        os._exit(fake_child())
    unittest.main()
