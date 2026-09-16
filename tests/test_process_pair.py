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
sys.path.insert(0, str(MODULE_PATH.parent))
SPEC = importlib.util.spec_from_file_location("avsync_process_pair", MODULE_PATH)
pair = importlib.util.module_from_spec(SPEC)
sys.modules[SPEC.name] = pair
SPEC.loader.exec_module(pair)


def native_summary(role: str) -> dict:
    result = {"schema": 1, "status": "control_stopped", "private_note": "DO_NOT_PUBLISH_PRIVATE_TEXT",
              "clock_epoch": "18446744073709551615", "sender_session": "PRIVATE_TOKEN"}
    if role == "sender":
        result.update({name: 0 for name in pair.SENDER_COUNTS})
        result.update(captured_packets=1000, captured_frames=(1 << 63) + 17, mapped_packets=900,
                      anchored_rtp_packets=800, rtp_packets_at_output=800, sender_reports=4,
                      clock_usable=True, original_anchors_transmitted=True)
    else:
        result.update({name: 0 for name in pair.RECEIVER_COUNTS})
        result.update(packets_accepted=804, ipc_published_frames=96000,
                      active_generation_usable_measurements=3,
                      historical_original_anchor_qualification=True, correction_diagnostic_pass=True,
                      sender_session_pinned=True, ipc_failure="none",
                      correction_sessions=[{"state": 1, "fault": 0, "delivered_frames": 96000}])
    return result


def fake_child() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fake-child", choices=("receiver", "sender"), required=True)
    parser.add_argument("--mode", required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--expect-sender-session")
    parser.add_argument("--sender-session")
    parser.add_argument("--clock-epoch")
    duration = parser.add_mutually_exclusive_group(required=True)
    duration.add_argument("--seconds")
    duration.add_argument("--session-seconds")
    parser.add_argument("--run-id")
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
                                     "clock": clock, "count": count, "run_id": args.run_id,
                                     "session_seconds": args.session_seconds,
                                     "at_ns": time.monotonic_ns()}) + "\n")

    record("start")
    message = {"schema": 1, "event": "receiver_ready" if role == "receiver" else "sender_started",
               "clock_epoch": clock, "sender_session": session}
    if mode == "stale_ack" and role == "sender":
        message["clock_epoch"] = str(int(clock) + 1)
    if mode == "foreign_session" and role == "receiver":
        message["sender_session"] = "1" if session != "1" else "2"
    line = b"AVSYNC_CONTROL " + json.dumps(message).encode("ascii") + b"\n"
    suppress = ((mode == "no_ready" or (mode in ("no_ready_once", "remote_hang_once") and count == 1)) and role == "receiver") or (mode in ("no_ack", "stderr_ack") and role == "sender")
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
    if ((mode in ("stdout_eof", "eof_bad_stop") and role == "sender")
            or (mode == "remote_stdout_eof" and role == "receiver")):
        os.close(sys.stdout.fileno())
    if mode == "remote_exit" and role == "receiver":
        return 0

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
            if mode == "ignore_stop" or (mode == "remote_hang_once" and role == "receiver" and count == 1):
                continue
            record("stop")
            if mode.startswith("native_"):
                summary = native_summary(role)
                if mode == "native_large":
                    summary["private_note"] *= 400
                elif mode == "native_oversize":
                    summary["private_note"] = "x" * (pair.MAX_NATIVE_SUMMARY + 1)
                elif mode == "native_nan":
                    summary["private_note"] = float("nan")
                elif mode == "native_bad_schema":
                    summary["schema"] = 2
                elif mode == "native_unqualified":
                    summary["clock_usable" if role == "sender" else "correction_diagnostic_pass"] = False
                elif mode == "native_error":
                    summary["status"] = "error"
                encoded = json.dumps(summary).encode("ascii")
                if mode == "native_truncated":
                    sys.stdout.buffer.write(encoded)
                else:
                    sys.stdout.buffer.write(encoded + b"\n")
                if mode == "native_duplicate":
                    sys.stdout.buffer.write(encoded + b"\n")
                if mode == "native_final_control":
                    sys.stdout.buffer.write(b"AVSYNC_CONTROL {invalid}\n")
                if mode == "native_total_overflow":
                    sys.stdout.buffer.write(b"ordinary log\n" * (pair.MAX_CHILD_OUTPUT // 12 + 2))
                sys.stdout.buffer.flush()
            return 7 if mode in ("bad_stop", "eof_bad_stop") else 0
        elif not command:
            record("eof")
            return 1
        else:
            record("invalid")
            return 1
    record("lease_expired")
    return 1


def fake_retirement() -> int:
    """Independent finite subprocess protocol fixture, not remote proof itself."""
    parser = argparse.ArgumentParser()
    parser.add_argument("--fake-retirement", action="store_true")
    parser.add_argument("--proof-mode", required=True)
    parser.add_argument("--state", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--expect-sender-session", dest="sender_session", required=True)
    parser.add_argument("--challenge", required=True)
    args = parser.parse_args()
    path = args.state / "retirement.log"
    previous = [json.loads(line) for line in path.read_text().splitlines()] if path.exists() else []
    current = {"run_id": args.run_id, "sender_session": args.sender_session,
               "challenge": args.challenge, "at_ns": time.monotonic_ns()}
    with path.open("a", encoding="ascii") as handle:
        handle.write(json.dumps(current) + "\n")
    mode = args.proof_mode
    if mode == "fail_second":
        mode = "nonzero" if previous else "valid"
    if mode == "replay_second":
        mode = "replay" if previous else "valid"
    proof = {"schema": 1, "event": "receiver_retired", "run_id": args.run_id,
             "sender_session": args.sender_session, "challenge": args.challenge,
             "proof": "fenced_empty_cgroup"}
    if mode in ("never_started", "prior_boot"):
        proof["proof"] = "fenced_" + mode
    elif mode in ("wrong_run", "wrong_challenge"):
        key = "run_id" if mode == "wrong_run" else "challenge"
        proof[key] = "0" * 32 if proof[key] != "0" * 32 else "1" * 32
    elif mode == "wrong_session":
        proof["sender_session"] = "1" if args.sender_session != "1" else "2"
    elif mode == "replay":
        proof.update({key: previous[0][key] for key in ("run_id", "sender_session", "challenge")})
    line = b"AVSYNC_RETIRE " + json.dumps(proof).encode("ascii") + b"\n"
    if mode in ("hang", "valid_then_hang"):
        if mode == "valid_then_hang":
            sys.stdout.buffer.write(line)
            sys.stdout.buffer.flush()
        time.sleep(30)  # The controller must kill/reap this exact bounded fixture.
        return 1
    if mode == "stderr_only":
        sys.stderr.buffer.write(line)
    else:
        if mode == "stdout_noise":
            sys.stdout.buffer.write(b"DO_NOT_PUBLISH_PRIVATE_PROOF_DIAGNOSTIC\n")
        sys.stdout.buffer.write(line[:-1] if mode == "truncated" else line)
        if mode == "duplicate":
            sys.stdout.buffer.write(line)
        if mode == "stdout_overflow":
            sys.stdout.buffer.write(b"x" * (1024 * 1024 + 1))
    if mode == "stderr_overflow":
        sys.stderr.buffer.write(b"x" * (1024 * 1024 + 1))
    if mode == "stderr_noise":
        sys.stderr.buffer.write(b"DO_NOT_PUBLISH_PRIVATE_PROOF_DIAGNOSTIC\n")
    sys.stdout.buffer.flush()
    sys.stderr.buffer.flush()
    return 7 if mode == "nonzero" else 0


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


def retirement_config(state: Path, mode: str = "healthy", proof_mode: str = "valid", **changes) -> dict:
    result = raw_config(state, mode, total_seconds=changes.pop("total_seconds", 5),
                        retire_timeout=changes.pop("retire_timeout", .8), **changes)
    result["receiver_scope"] = "remote"
    result["receiver_argv"] += ["--run-id", "{run_id}"]
    result["retire_argv"] = [sys.executable, str(Path(__file__).resolve()), "--fake-retirement",
                             "--proof-mode", proof_mode, "--state", str(state),
                             "--run-id", "{run_id}", "--expect-sender-session", "{sender_session}",
                             "--challenge", "{challenge}"]
    return result


def session_config(base: dict, **changes) -> dict:
    result = {**base, "session_mode": True, "max_attempts": 1, **changes}
    for role in ("receiver", "sender"):
        result[role + "_argv"] = ["--session-seconds" if arg == "--seconds" else arg
                                  for arg in result[role + "_argv"]]
    return result


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

    def test_recoverable_stdin_fault_cannot_mask_later_terminal_output_fault(self):
        child = object.__new__(pair.Child)
        child.failure = None
        child.output_failure = None
        child._fail("stdin_write_failed")
        child._fail("output_total_exceeded")
        child._fail("stdout_read_failed")
        self.assertEqual(child.failure, "stdin_write_failed")
        self.assertEqual(child.output_failure, "output_total_exceeded")

    def test_invalid_configs(self):
        base = raw_config(Path("fixture"))
        variants = [{**base, "max_attempts": x} for x in (0, 4, True)]
        variants += [{**base, "total_seconds": x} for x in (0, 181, float("nan"), float("inf"), True)]
        variants += [{**base, "stop_grace": 0}, {**base, "extra": 1},
                     {**base, "receiver_argv": "arbitrary shell"}]
        variants += [{**base, "require_native_summaries": value} for value in (1, None, "true")]
        variants += [{**base, "receiver_scope": value} for value in ("unknown", None, True)]
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

    def test_session_mode_is_explicit_bounded_and_one_attempt(self):
        raw = session_config(raw_config(Path("fixture")), total_seconds=43200)
        config = pair.Config.from_dict(raw)
        self.assertTrue(config.session_mode)
        self.assertEqual(config.total_seconds, 43200)
        self.assertEqual(config.max_attempts, 1)
        for role in ("receiver", "sender"):
            argv = config.argv(role, "123", "456", 43200)
            self.assertNotIn("--seconds", argv)
            self.assertEqual(argv[argv.index("--session-seconds") + 1], "43200")
        del raw["max_attempts"]
        self.assertEqual(pair.Config.from_dict(raw).max_attempts, 1)
        self.assertFalse(pair.Config.from_dict(raw_config(Path("fixture"))).session_mode)

    def test_session_mode_rejects_implicit_mixed_or_recovering_configuration(self):
        base = session_config(raw_config(Path("fixture")))
        variants = [{**base, "session_mode": value} for value in (False, None, 1, "true")]
        variants += [{**base, "max_attempts": value} for value in (2, 3, True)]
        variants += [{**base, "total_seconds": value} for value in (43201, 0, float("inf"), True)]
        for role in ("receiver", "sender"):
            variants.append({**base, role + "_argv": raw_config(Path("fixture"))[role + "_argv"]})
            for extra in (["--seconds", "{seconds}"], ["--seconds=180"], ["--session-seconds=43200"],
                          ["--session-seconds", "{seconds}"], ["{seconds}"]):
                variants.append({**base, role + "_argv": base[role + "_argv"] + extra})
        for extra in (["--recover-desktop-ipc"], ["--clock-pause-after", "5"], ["--clock-pause-seconds=2"]):
            variants.append({**base, "receiver_argv": base["receiver_argv"] + extra})
        for value in variants:
            with self.subTest(value=value), self.assertRaises(pair.PairError):
                pair.Config.from_dict(value)

    def test_remote_session_requires_independent_retirement(self):
        base = session_config(raw_config(Path("fixture")), receiver_scope="remote")
        with self.assertRaisesRegex(pair.PairError, "session_remote_retirement_required"):
            pair.Config.from_dict(base)
        config = pair.Config.from_dict(session_config(retirement_config(Path("fixture")), total_seconds=43200))
        self.assertIsNotNone(config.retire_argv)
        self.assertEqual(config.max_attempts, 1)

    def test_known_ssh_requires_explicit_remote_scope(self):
        for executable in ("ssh", "ssh.exe", "/usr/bin/ssh", "C:\\Windows\\System32\\OpenSSH\\ssh.exe"):
            base = raw_config(Path("fixture"))
            base["receiver_argv"][0] = executable
            with self.subTest(executable=executable), self.assertRaises(pair.PairError):
                pair.Config.from_dict(base)
            self.assertEqual(pair.Config.from_dict({**base, "receiver_scope": "remote"}).receiver_scope, "remote")

    def test_retirement_configuration_exact_argument_substitution(self):
        config = pair.Config.from_dict(retirement_config(Path("private fixture")))
        run_id, session, challenge = "a" * 32, "9007199254740993", "b" * 32
        receiver = config.argv("receiver", session, None, 4, run_id=run_id)
        self.assertEqual(receiver[receiver.index("--run-id") + 1], run_id)
        query = config.retirement_argv(run_id, session, challenge)
        for flag, expected in (("--run-id", run_id), ("--expect-sender-session", session),
                               ("--challenge", challenge)):
            self.assertEqual(query[query.index(flag) + 1], expected)
        self.assertIn("private fixture", query)
        self.assertIsInstance(config.retire_argv, tuple)

    def test_invalid_retirement_configurations(self):
        base = retirement_config(Path("fixture"))
        variants = [{**base, "receiver_scope": "direct"}, {**base, "retire_argv": []},
                    {**base, "retire_argv": "shell command"}]
        variants += [{**base, "retire_timeout": value} for value in
                     (0, .01, 10.01, True, None, float("nan"), float("inf"))]
        variants += [{**base, "receiver_argv": base["receiver_argv"][:-2]},
                     {**base, "receiver_argv": base["receiver_argv"] + ["--run-id", "{run_id}"]}]
        for token in ("{run_id}", "{sender_session}", "{challenge}"):
            variants.append({**base, "retire_argv": [arg for arg in base["retire_argv"] if arg != token]})
        for arg in ("--challenge={challenge}", "{clock_epoch}", "{seconds}", "\nprivate", "\0"):
            variants.append({**base, "retire_argv": base["retire_argv"] + [arg]})
        variants += [{**raw_config(Path("fixture")), "retire_timeout": .8},
                     {**raw_config(Path("fixture")), "receiver_scope": "remote",
                      "receiver_argv": base["receiver_argv"]}]
        for value in variants:
            with self.subTest(value=value), self.assertRaises(pair.PairError):
                pair.Config.from_dict(value)

    def test_native_summary_preserves_uint64_without_private_fields(self):
        for role in ("receiver", "sender"):
            original = native_summary(role)
            original["private_note"] *= 400
            encoded = json.dumps(original).encode()
            self.assertGreater(len(encoded), pair.MAX_LINE)
            summary = pair.parse_native_summary(encoded, role)
            self.assertTrue(summary["reported_media_qualified"])
            serialized = json.dumps(summary)
            for private in ("DO_NOT_PUBLISH", "PRIVATE_TOKEN", "clock_epoch", '"sender_session":', "private_note"):
                self.assertNotIn(private, serialized)
            if role == "sender":
                self.assertEqual(summary["metrics"]["captured_frames"], (1 << 63) + 17)
                self.assertIs(type(summary["metrics"]["captured_frames"]), int)

    def test_native_summary_rejects_size_schema_duplicates_and_nonfinite(self):
        base = native_summary("sender")
        variants = [{**base, "schema": True}, {**base, "schema": 2},
                    {**base, "status": "DO_NOT_PUBLISH_PRIVATE_ERROR"}, {**base, "status": []},
                    {**base, "mapped_packets": True}, {**base, "mapped_packets": -1},
                    {**base, "mapped_packets": 1 << 64}, {**base, "clock_usable": 1},
                    {**base, "unknown": float("nan")}, {**base, "unknown": float("inf")},
                    [base], None]
        lines = [json.dumps(value).encode() for value in variants]
        lines += [b'{"schema":1,"schema":1,"status":"error"}', b"{bad}", b"{\xff}",
                  b'{"schema":1,"status":"error","unknown":1e999}', b"{" * (pair.MAX_NATIVE_SUMMARY + 1)]
        for line in lines:
            with self.subTest(line=line[:40]), self.assertRaises(pair.PairError):
                pair.parse_native_summary(line, "sender")

    def test_receiver_requires_exact_native_no_ipc_failure(self):
        base = native_summary("receiver")
        summary = pair.parse_native_summary(json.dumps(base).encode(), "receiver")
        self.assertTrue(summary["reported_media_qualified"])
        self.assertEqual(summary["ipc_failure"], "none")
        variants = [{key: value for key, value in base.items() if key != "ipc_failure"}]
        variants += [{**base, "ipc_failure": value} for value in
                     ("", "None", None, True, {}, "DO_NOT_PUBLISH_PRIVATE_ERROR", *sorted(pair.IPC_FAILURE_CODES - {"none"}))]
        for original in variants:
            with self.subTest(failure=original.get("ipc_failure")):
                summary = pair.parse_native_summary(json.dumps(original).encode(), "receiver")
                self.assertFalse(summary["reported_media_qualified"])
                value = original.get("ipc_failure")
                expected = value if isinstance(value, str) and value in pair.IPC_FAILURE_CODES else "unknown"
                self.assertEqual(summary["ipc_failure"], expected)
                self.assertNotIn("DO_NOT_PUBLISH", json.dumps(summary))

    def test_receiver_session_diagnostics_are_bounded_and_sanitized(self):
        base = native_summary("receiver")
        original = {"state": 2, "fault": 5, "delivered_frames": (1 << 63) + 17,
                    "generation": "PRIVATE_GENERATION", "ssrc": "PRIVATE_SSRC", "private_note": "DO_NOT_PUBLISH"}
        summary = pair.parse_native_summary(json.dumps({**base, "correction_sessions": [original]}).encode(), "receiver")
        self.assertFalse(summary["reported_media_qualified"])
        self.assertEqual(summary["correction_sessions"], [{"state": 2, "fault": 5, "delivered_frames": (1 << 63) + 17}])
        self.assertEqual(summary["correction_session_count"], 1)
        self.assertFalse(summary["correction_sessions_truncated"])
        self.assertNotIn("PRIVATE", json.dumps(summary))
        self.assertNotIn("DO_NOT_PUBLISH", json.dumps(summary))
        valid = base["correction_sessions"][0]
        for sessions in (None, {}, [], [None], [{"state": True, "fault": "PRIVATE", "delivered_frames": 1 << 64}],
                         [{**valid, "delivered_frames": -1}], [valid] * (pair.MAX_CORRECTION_SESSIONS + 1)):
            with self.subTest(sessions=sessions):
                summary = pair.parse_native_summary(json.dumps({**base, "correction_sessions": sessions}).encode(), "receiver")
                self.assertFalse(summary["reported_media_qualified"])
                self.assertLessEqual(len(summary["correction_sessions"]), pair.MAX_CORRECTION_SESSIONS)
                self.assertEqual(summary["correction_session_count"], len(sessions) if isinstance(sessions, list) else None)
                self.assertEqual(summary["correction_sessions_truncated"], isinstance(sessions, list) and len(sessions) > pair.MAX_CORRECTION_SESSIONS)
                self.assertNotIn("PRIVATE", json.dumps(summary))

    def test_stop_and_missing_counters_do_not_imply_reported_media_qualification(self):
        for role in ("sender", "receiver"):
            summary = pair.parse_native_summary(b'{"schema":1,"status":"control_stopped"}', role)
            self.assertFalse(summary["reported_media_qualified"])
            original = native_summary(role)
            original["status"] = "error"
            self.assertFalse(pair.parse_native_summary(json.dumps(original).encode(), role)["reported_media_qualified"])

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


class RetirementProofTests(unittest.TestCase):
    run_id = "a" * 32
    session = "9007199254740993"
    challenge = "b" * 32

    def proof_line(self, **changes):
        message = {"schema": 1, "event": "receiver_retired", "run_id": self.run_id,
                   "sender_session": self.session, "challenge": self.challenge,
                   "proof": "fenced_empty_cgroup"}
        return b"AVSYNC_RETIRE " + json.dumps({**message, **changes}).encode("ascii") + b"\n"

    def parse(self, line):
        return pair.parse_retirement(line, self.run_id, self.session, self.challenge)

    def test_only_exact_fenced_proofs_with_bound_attempt_identity(self):
        for kind in ("fenced_empty_cgroup", "fenced_never_started", "fenced_prior_boot"):
            self.assertEqual(self.parse(self.proof_line(proof=kind)), kind)
        variants = [{"schema": value} for value in (True, 0, 2, "1")]
        variants += [{"event": "receiver_ready"}, {"proof": "not_running"},
                     {"proof": "ssh_exited"}, {"proof": True}, {"private": "extra"}]
        for key, values in (("run_id", ("c" * 32, "A" * 32, "a" * 31, 123)),
                             ("challenge", ("c" * 32, "B" * 32, "b" * 33, None)),
                             ("sender_session", ("7", int(self.session), "0", "01", str(1 << 64)))):
            variants += [{key: value} for value in values]
        for variant in variants:
            with self.subTest(variant=variant), self.assertRaises(ValueError):
                self.parse(self.proof_line(**variant))

    def test_malformed_or_duplicate_proof_rejected(self):
        valid = self.proof_line()
        variants = (valid.replace(b'"schema": 1', b'"schema":1,"schema":1'),
                    valid.replace(b'"schema": 1', b'"schema":NaN'),
                    b"AVSYNC_RETIRE \xff", b"AVSYNC_RETIRE {", b"AVSYNC_RETIRE []",
                    b"AVSYNC_RETIREX {}", b"AVSYNC_RETIRE " + b"x" * 4097,
                    valid + b"\n" + valid)
        for value in variants:
            with self.subTest(value=value[:50]), self.assertRaises(ValueError):
                self.parse(value)

    def verify_fixture(self, mode, timeout=.8):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        state = Path(directory.name)
        argv = retirement_config(state, proof_mode=mode)["retire_argv"]
        substitutions = {"{run_id}": self.run_id, "{sender_session}": self.session,
                         "{challenge}": self.challenge}
        argv = [substitutions.get(arg, arg) for arg in argv]
        before = time.monotonic()
        result = pair.verify_retirement(argv, self.run_id, self.session, self.challenge, timeout)
        self.assertLess(time.monotonic() - before, timeout + .75, result)
        serialized = json.dumps(result)
        for private in (self.run_id, self.session, self.challenge, str(state), "DO_NOT_PUBLISH"):
            self.assertNotIn(private, serialized)
        return result

    def test_fresh_independent_process_complete_exit_and_bounded_stderr(self):
        for mode, proof in (("valid", "fenced_empty_cgroup"), ("stderr_noise", "fenced_empty_cgroup"),
                            ("never_started", "fenced_never_started"), ("prior_boot", "fenced_prior_boot")):
            with self.subTest(mode=mode):
                result = self.verify_fixture(mode)
                self.assertTrue(result["verified"], result)
                self.assertEqual(result["proof"], proof)
                self.assertFalse(result["forced_local_kill"], result)

    def test_nonzero_partial_replayed_or_malicious_output_never_proves_retirement(self):
        for mode in ("nonzero", "duplicate", "stdout_noise", "truncated", "wrong_run",
                     "wrong_session", "wrong_challenge", "stderr_only", "stdout_overflow", "stderr_overflow"):
            with self.subTest(mode=mode):
                result = self.verify_fixture(mode)
                self.assertFalse(result["verified"], result)
                self.assertIsNone(result["proof"], result)
                self.assertIsInstance(result["reason"], str)

    def test_valid_line_without_successful_exit_is_not_proof(self):
        for mode in ("hang", "valid_then_hang"):
            with self.subTest(mode=mode):
                result = self.verify_fixture(mode, timeout=.4)
                self.assertFalse(result["verified"], result)
                self.assertTrue(result["forced_local_kill"], result)
                self.assertIsNone(result["proof"], result)

    def test_invalid_query_budget_never_spawns_a_process(self):
        from unittest.mock import patch
        for timeout in (0, -.1, True, float("inf"), float("nan"), 10.1):
            with self.subTest(timeout=timeout), patch.object(subprocess, "Popen") as spawn:
                result = pair.verify_retirement(["unused"], self.run_id, self.session, self.challenge, timeout)
                self.assertFalse(result["verified"])
                spawn.assert_not_called()


class ProcessTests(unittest.TestCase):
    def assertEqual(self, first, second, msg=None):
        # Include retained protocol/cleanup history in every fixture assertion.
        if msg is None and hasattr(self, "last_result"):
            msg = self.last_result
        return super().assertEqual(first, second, msg)

    def run_fixture(self, mode="healthy", proof_mode=None, **changes):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        state = Path(directory.name)
        raw = (raw_config(state, mode, **changes) if proof_mode is None else
               retirement_config(state, mode, proof_mode, **changes))
        if changes.get("session_mode") is True:
            raw = session_config(raw, max_attempts=changes.get("max_attempts", 1))
        config = pair.Config.from_dict(raw)
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
                if expected is None and mode == "masked_output_failure" and attempt not in self.injected_attempts:
                    if self.result.acknowledged_pairs < attempt:
                        raise AssertionError("output failure injected before complete pair agreement")
                    target = next(child for child in self.children if child.role == "receiver")
                    target._fail("stdin_write_failed")
                    target._fail("output_total_exceeded")
                    self.injected_attempts.add(attempt)
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

    def test_session_uses_session_native_flag_and_cooperative_stop(self):
        result, state = self.run_fixture(session_mode=True)
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.attempts, 1)
        for role in ("receiver", "sender"):
            self.assertTrue(all(event["session_seconds"] is not None for event in events(state, role)))
            self.assertEqual(events(state, role)[-1]["event"], "stop")

    def test_session_fault_never_retries_even_with_verified_retirement(self):
        # The acknowledged-pair fault barrier ends immediately; no hours-long
        # wait is performed. Verify the native child budget is not capped at180.
        result, state = self.run_fixture("receiver_dies_once", proof_mode="valid", session_mode=True,
                                         total_seconds=43200)
        self.assertNotEqual(result.exit_code, 0)
        self.assertEqual(result.attempts, 1)
        self.assertEqual(len(result.remote_retirement_checks), 1)
        self.assertTrue(result.remote_retirement_checks[0]["verified"])
        for role in ("receiver", "sender"):
            self.assertEqual(len([event for event in events(state, role) if event["event"] == "start"]), 1)
            self.assertTrue(180 < int(events(state, role)[0]["session_seconds"]) <= 43200)

    def test_required_large_native_summaries_are_collected_during_stop(self):
        result, _ = self.run_fixture("native_large", require_native_summaries=True)
        self.assertEqual(result.exit_code, 0)
        self.assertTrue(result.native_summary_requirement_met)
        self.assertTrue(result.reported_media_qualified)
        self.assertFalse(result.media_verified)
        self.assertEqual(result.evidence, "process_agreement_only")
        self.assertEqual(len(result.native_diagnostics), 2)
        self.assertEqual({item["role"] for item in result.native_diagnostics}, {"receiver", "sender"})
        receiver = next(item for item in result.native_diagnostics if item["role"] == "receiver")
        self.assertEqual(receiver["ipc_failure"], "none")
        self.assertEqual(receiver["correction_sessions"], [{"state": 1, "fault": 0, "delivered_frames": 96000}])
        self.assertEqual(receiver["correction_session_count"], 1)
        self.assertFalse(receiver["correction_sessions_truncated"])
        serialized = json.dumps(pair.asdict(result))
        for private in ("DO_NOT_PUBLISH", "PRIVATE_TOKEN", "private_note", "clock_epoch"):
            self.assertNotIn(private, serialized)

    def test_required_missing_summary_does_not_imply_media_pass(self):
        result, _ = self.run_fixture(require_native_summaries=True)
        self.assertEqual(result.exit_code, 0)  # Control result remains independent.
        self.assertFalse(result.native_summary_requirement_met)
        self.assertFalse(result.reported_media_qualified)
        self.assertTrue(all(item["reason"] == "native_summary_missing" for item in result.native_diagnostics))

    def test_invalid_final_native_summaries_have_separate_diagnostics(self):
        for mode, reason in (("native_duplicate", "duplicate_native_summary"),
                             ("native_bad_schema", "invalid_native_schema"),
                             ("native_nan", "invalid_native_json")):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, require_native_summaries=True)
                self.assertEqual(result.exit_code, 0)
                self.assertFalse(result.native_summary_requirement_met)
                self.assertFalse(result.reported_media_qualified)
                self.assertTrue(all(item["reason"] == reason for item in result.native_diagnostics))

    def test_valid_error_or_unqualified_summaries_are_not_qualified(self):
        for mode in ("native_error", "native_unqualified"):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, require_native_summaries=True)
                self.assertEqual(result.exit_code, 0)
                self.assertTrue(result.native_summary_requirement_met)
                self.assertFalse(result.reported_media_qualified)

    def test_bad_final_stdout_is_not_lost_during_cleanup(self):
        for mode in ("native_truncated", "native_oversize", "native_final_control", "native_total_overflow"):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, require_native_summaries=True)
                self.assertEqual(result.exit_code, 1)
                self.assertTrue(any(f.phase == "cleanup" for f in result.faults))
                if mode != "native_final_control":
                    self.assertFalse(result.reported_media_qualified)

    def test_remote_receiver_death_never_retries_from_local_ssh_exit_alone(self):
        result, state = self.run_fixture("receiver_dies_once", receiver_scope="remote", max_attempts=3)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(result.remote_retirement_unverified)
        self.assertEqual((state / "receiver.count").read_text(), "1")
        self.assertEqual((state / "sender.count").read_text(), "1")
        self.assertEqual(events(state, "sender")[-1]["event"], "stop")

    def test_remote_receiver_exit_or_stdout_loss_never_retries(self):
        for mode in ("remote_exit", "remote_stdout_eof"):
            with self.subTest(mode=mode):
                result, state = self.run_fixture(mode, receiver_scope="remote", max_attempts=3)
                self.assertEqual(result.exit_code, 1)
                self.assertEqual(result.attempts, 1)
                self.assertTrue(result.remote_retirement_unverified)
                self.assertEqual((state / "receiver.count").read_text(), "1")

    def test_remote_healthy_stop_still_requires_independent_proof(self):
        result, state = self.run_fixture(proof_mode="valid")
        self.assertEqual(result.exit_code, 0)
        self.assertEqual(result.attempts, 1)
        self.assertFalse(result.remote_retirement_unverified)
        self.assertEqual(len(result.remote_retirement_checks), 1)
        self.assertTrue(result.remote_retirement_checks[0]["verified"])
        queries = events(state, "retirement")
        receiver = events(state, "receiver")
        self.assertEqual(len(queries), 1)
        self.assertEqual(queries[0]["run_id"], receiver[0]["run_id"])
        self.assertEqual(queries[0]["sender_session"], receiver[0]["session"])
        self.assertGreater(queries[0]["at_ns"], receiver[-1]["at_ns"])
        serialized = json.dumps(pair.asdict(result))
        for private in (queries[0]["run_id"], queries[0]["sender_session"], queries[0]["challenge"], str(state)):
            self.assertNotIn(private, serialized)

    def test_verified_remote_retirement_enables_fresh_retry_without_erasing_fault(self):
        result, state = self.run_fixture("receiver_dies_once", proof_mode="valid", total_seconds=7)
        self.assertEqual(result.exit_code, 3)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.acknowledged_pairs, 2)
        self.assertFalse(result.remote_retirement_unverified)
        self.assertEqual(len(result.remote_retirement_checks), 2)
        self.assertTrue(all(item["verified"] for item in result.remote_retirement_checks))
        self.assertTrue(any(fault.attempt == 1 and fault.reason.startswith("receiver_") for fault in result.faults))
        starts = [item for item in events(state, "receiver") if item["event"] == "start"]
        queries = events(state, "retirement")
        self.assertEqual(len(starts), 2)
        self.assertEqual(len(queries), 2)
        self.assertGreater(starts[1]["at_ns"], queries[0]["at_ns"])
        for key in ("run_id", "session", "clock"):
            self.assertNotEqual(starts[0][key], starts[1][key])
        self.assertNotEqual(queries[0]["challenge"], queries[1]["challenge"])
        for start, query in zip(starts, queries):
            self.assertEqual(start["run_id"], query["run_id"])
            self.assertEqual(start["session"], query["sender_session"])

    def test_unknown_startup_window_is_fenced_before_retry(self):
        result, state = self.run_fixture("no_ready_once", proof_mode="never_started",
                                         ready_timeout=.6, total_seconds=7)
        self.assertEqual(result.exit_code, 3)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.acknowledged_pairs, 1)
        self.assertTrue(any(f.reason == "receiver_ready_timeout" for f in result.faults))
        self.assertEqual(len(result.remote_retirement_checks), 2)
        self.assertTrue(all(item["verified"] for item in result.remote_retirement_checks))
        starts = [item for item in events(state, "receiver") if item["event"] == "start"]
        queries = events(state, "retirement")
        self.assertEqual(queries[0]["run_id"], starts[0]["run_id"])
        self.assertGreater(starts[1]["at_ns"], queries[0]["at_ns"])
        self.assertEqual((state / "sender.count").read_text(), "1")

    def test_failed_proof_without_ready_never_launches_second_receiver(self):
        result, state = self.run_fixture("no_ready_once", proof_mode="nonzero", ready_timeout=.6)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(result.remote_retirement_unverified)
        self.assertEqual(len(result.remote_retirement_checks), 1)
        self.assertFalse(result.remote_retirement_checks[0]["verified"])
        self.assertFalse((state / "sender.count").exists())
        self.assertEqual((state / "receiver.count").read_text(), "1")

    def test_fence_is_required_even_when_receiver_transport_spawn_raises(self):
        from unittest.mock import patch
        original_child = pair.Child
        for proof_mode, expected_exit in (("never_started", 3), ("nonzero", 1)):
            with self.subTest(proof_mode=proof_mode):
                failed_once = False

                def child_factory(role, argv):
                    nonlocal failed_once
                    if role == "receiver" and not failed_once:
                        failed_once = True
                        raise pair.PairError("receiver_spawn_failed")
                    return original_child(role, argv)

                with patch.object(pair, "Child", side_effect=child_factory):
                    result, state = self.run_fixture(proof_mode=proof_mode)
                self.assertEqual(result.exit_code, expected_exit)
                self.assertEqual(result.attempts, 2 if expected_exit == 3 else 1)
                self.assertTrue(any(f.reason == "receiver_spawn_failed" for f in result.faults))
                self.assertEqual(len(result.remote_retirement_checks), result.attempts)
                queries = events(state, "retirement")
                self.assertEqual(len(queries), result.attempts)
                if expected_exit == 3:
                    start = next(item for item in events(state, "receiver") if item["event"] == "start")
                    self.assertNotEqual(queries[0]["run_id"], start["run_id"])
                    self.assertGreater(start["at_ns"], queries[0]["at_ns"])
                else:
                    self.assertFalse((state / "receiver.count").exists())

    def test_failed_proof_also_rejects_otherwise_healthy_stop(self):
        result, _ = self.run_fixture(proof_mode="nonzero")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(result.remote_retirement_unverified)
        self.assertEqual(len(result.remote_retirement_checks), 1)
        self.assertFalse(result.remote_retirement_checks[0]["verified"])

    def test_old_proof_cannot_retire_successor_and_failed_second_proof_is_terminal(self):
        for mode in ("fail_second", "replay_second"):
            with self.subTest(mode=mode):
                result, state = self.run_fixture("receiver_dies_once", proof_mode=mode, total_seconds=7)
                self.assertEqual(result.exit_code, 1)
                self.assertEqual(result.attempts, 2)
                self.assertTrue(result.remote_retirement_unverified)
                self.assertEqual(len(result.remote_retirement_checks), 2)
                self.assertTrue(result.remote_retirement_checks[0]["verified"])
                self.assertFalse(result.remote_retirement_checks[1]["verified"])
                self.assertEqual((state / "receiver.count").read_text(), "2")

    def test_verified_remote_fence_allows_only_exact_receiver_transport_kill(self):
        result, _ = self.run_fixture("remote_hang_once", proof_mode="valid", ready_timeout=.6,
                                     total_seconds=7)
        self.assertEqual(result.exit_code, 3)
        self.assertEqual(result.attempts, 2)
        self.assertEqual(result.forced_local_kills, 1)
        self.assertTrue(result.faults)
        self.assertTrue(all(item["verified"] for item in result.remote_retirement_checks))

    def test_remote_proof_never_excuses_sender_forced_kill(self):
        result, _ = self.run_fixture("ignore_stop", proof_mode="valid")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertEqual(result.forced_local_kills, 2)
        self.assertTrue(result.remote_retirement_checks[0]["verified"])

    def test_remote_proof_never_excuses_identity_or_output_corruption(self):
        for mode in ("foreign_session", "stale_ack", "duplicate_ready", "long_line"):
            with self.subTest(mode=mode):
                result, _ = self.run_fixture(mode, proof_mode="valid", max_attempts=3)
                self.assertEqual(result.exit_code, 1)
                self.assertEqual(result.attempts, 1)
                self.assertTrue(result.remote_retirement_checks[0]["verified"])

    def test_remote_proof_cannot_excuse_terminal_output_fault_masked_by_stdin_failure(self):
        result, state = self.run_fixture("masked_output_failure", proof_mode="valid")
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(result.remote_retirement_checks[0]["verified"])
        self.assertTrue(any(f.reason == "receiver_stdin_write_failed" for f in result.faults))
        self.assertTrue(any(f.reason == "receiver_output_total_exceeded" for f in result.faults))
        self.assertEqual((state / "receiver.count").read_text(), "1")

    def test_remote_proof_timeout_stays_inside_overall_budget_and_never_retries(self):
        result, state = self.run_fixture("no_ready_once", proof_mode="valid_then_hang", ready_timeout=.6,
                                         retire_timeout=.4, total_seconds=4)
        self.assertEqual(result.exit_code, 1)
        self.assertEqual(result.attempts, 1)
        self.assertTrue(result.remote_retirement_unverified)
        self.assertTrue(result.remote_retirement_checks[0]["forced_local_kill"])
        self.assertFalse(result.remote_retirement_checks[0]["verified"])
        self.assertEqual((state / "receiver.count").read_text(), "1")

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
    if "--fake-retirement" in sys.argv:
        os._exit(fake_retirement())
    if "--fake-child" in sys.argv:
        # Model a native process exit, without CPython waiting on the daemon
        # thread that deliberately blocks in the stdin fixture.
        os._exit(fake_child())
    unittest.main()
