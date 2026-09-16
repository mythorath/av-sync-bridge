#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Finite desktop process control, with explicit supervised-session opt-in.

Default diagnostics remain bounded to 180 seconds. Sessions permit at most
twelve hours and exactly one process pair; they do not enable automatic recovery
or turn process agreement into audio-health evidence.
"""

from __future__ import annotations

import argparse
from dataclasses import asdict, dataclass, field
import json
import math
import os
from pathlib import Path
import queue
import re
import secrets
import stat
import subprocess
import sys
import threading
import time
from typing import Any
from retirement_proof import parse_retirement, verify_retirement


PREFIX = b"AVSYNC_CONTROL "
MAX_LINE = 4096
MAX_NATIVE_SUMMARY = 65536
MAX_CHILD_OUTPUT = 1024 * 1024
MAX_CONTROL = 1024
MAX_PENDING = 64
UINT64_MAX = (1 << 64) - 1
KEEPALIVE = b"AVSYNC_KEEPALIVE\n"
STOP = b"AVSYNC_STOP\n"
OUTPUT_FAILURES = {
    "output_total_exceeded", "output_line_too_long", "truncated_stdout_line",
    "stdout_read_failed", "stderr_read_failed", "output_not_drained", "control_output_overflow",
}


class PairError(Exception):
    """An intentionally non-sensitive, bounded reason code."""


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("duplicate_key")
        result[key] = value
    return result


def _identity(value: Any) -> str:
    if not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", value):
        raise PairError("invalid_identity")
    if int(value) > UINT64_MAX:
        raise PairError("invalid_identity")
    return value


def parse_control(line: bytes) -> dict[str, Any]:
    if len(line) > MAX_CONTROL or not line.startswith(PREFIX):
        raise PairError("invalid_control_line")
    try:
        message = json.loads(line[len(PREFIX):].decode("ascii"),
                             object_pairs_hook=_unique_object)
    except (UnicodeError, ValueError, RecursionError):
        raise PairError("invalid_control_json") from None
    if not isinstance(message, dict) or set(message) != {
        "schema", "event", "clock_epoch", "sender_session"
    }:
        raise PairError("invalid_control_schema")
    if type(message["schema"]) is not int or message["schema"] != 1:
        raise PairError("invalid_control_schema")
    if message["event"] not in ("receiver_ready", "sender_started"):
        raise PairError("invalid_control_event")
    _identity(message["clock_epoch"])
    _identity(message["sender_session"])
    return message


NATIVE_STATUSES = {
    "control_stopped", "control_eof", "control_expired", "control_invalid", "control_read_error",
    "error", "waiting_clock", "waiting_no_rtp", "invalid_nominal_timing",
    "rtp_output_observed_unverified", "original_anchors_observed", "recovered_with_gap",
    "awaiting_media_generation", "correction_unqualified", "original_anchors_unqualified",
    "waiting_media", "priming_reference", "invalid_timing", "timestamps_observed",
}
SENDER_COUNTS = (
    "captured_packets", "captured_frames", "mapped_packets", "anchored_rtp_packets",
    "rtp_packets_at_output", "sender_reports", "timestamp_errors", "queue_overflows",
    "rtp_frame_steps", "rtp_nominal_pts_steps", "rtp_invalid_payload", "anchor_transport_errors",
    "clock_loss_count", "resets", "reject_unhealthy", "dropped_packets",
)
RECEIVER_COUNTS = (
    "packets_accepted", "packets_rejected", "invalid_ingress_anchor_packets",
    "foreign_identity_rtp_packets", "unadmitted_rtcp_packets", "invalid_sender_reports",
    "active_generation_usable_measurements", "ipc_published_frames", "ipc_busy_retries",
    "ipc_retry_queue_peak_blocks", "ipc_recreations", "jitter_num_too_late", "jitter_num_drop_on_latency",
)
IPC_FAILURE_CODES = {
    "none", "retry_deadline", "ipc_write_rejected", "invalid_time",
    "ipc_heartbeat_rejected", "invalid_block_timestamp_or_capacity",
}
MAX_CORRECTION_SESSIONS = 8


def _reject_constant(_: str):
    raise ValueError("nonfinite_number")


def parse_native_summary(line: bytes, role: str) -> dict[str, Any]:
    """Retain only compact allowlisted diagnostics, never arbitrary native text."""
    if len(line) > MAX_NATIVE_SUMMARY:
        raise PairError("native_summary_too_large")
    try:
        raw = json.loads(line.decode("utf-8"), object_pairs_hook=_unique_object,
                         parse_constant=_reject_constant)
        pending, inspected = [raw], 0
        while pending:
            value = pending.pop()
            inspected += 1
            if inspected > 32768 or (type(value) is float and not math.isfinite(value)):
                raise ValueError("invalid_number_or_structure")
            if isinstance(value, dict):
                pending.extend(value.values())
            elif isinstance(value, list):
                pending.extend(value)
    except (UnicodeError, ValueError, RecursionError):
        raise PairError("invalid_native_json") from None
    if (not isinstance(raw, dict) or type(raw.get("schema")) is not int or raw["schema"] != 1
            or not isinstance(raw.get("status"), str) or raw["status"] not in NATIVE_STATUSES):
        raise PairError("invalid_native_schema")
    metrics: dict[str, int | bool] = {}
    counts = SENDER_COUNTS if role == "sender" else RECEIVER_COUNTS
    booleans = (("clock_usable", "original_anchors_transmitted") if role == "sender" else
                ("historical_original_anchor_qualification", "correction_diagnostic_pass", "sender_session_pinned"))
    for name in counts:
        if name in raw:
            if type(raw[name]) is not int or not 0 <= raw[name] <= UINT64_MAX:
                raise PairError("invalid_native_metric")
            metrics[name] = raw[name]
    for name in booleans:
        if name in raw:
            if type(raw[name]) is not bool:
                raise PairError("invalid_native_metric")
            metrics[name] = raw[name]
    details: dict[str, Any] = {}
    good_status = raw["status"] in ("control_stopped", "rtp_output_observed_unverified", "original_anchors_observed")
    if role == "sender":
        qualified = (good_status and metrics.get("clock_usable") is True
                     and metrics.get("original_anchors_transmitted") is True
                     and all(metrics.get(name, 0) > 0 for name in
                             ("captured_packets", "mapped_packets", "anchored_rtp_packets", "rtp_packets_at_output", "sender_reports"))
                     and all(metrics.get(name) == 0 for name in
                             ("timestamp_errors", "queue_overflows", "rtp_frame_steps", "rtp_nominal_pts_steps",
                              "rtp_invalid_payload", "anchor_transport_errors", "clock_loss_count")))
    else:
        sessions = raw.get("correction_sessions")
        safe_sessions: list[dict[str, int | None]] = []
        if isinstance(sessions, list):
            for item in sessions[:MAX_CORRECTION_SESSIONS]:
                safe_item = {}
                for name in ("state", "fault", "delivered_frames"):
                    value = item.get(name) if isinstance(item, dict) else None
                    safe_item[name] = value if type(value) is int and 0 <= value <= UINT64_MAX else None
                safe_sessions.append(safe_item)
        failure = raw.get("ipc_failure")
        details = {
            "ipc_failure": failure if isinstance(failure, str) and failure in IPC_FAILURE_CODES else "unknown",
            "correction_sessions": safe_sessions,
            "correction_session_count": len(sessions) if isinstance(sessions, list) else None,
            "correction_sessions_truncated": isinstance(sessions, list) and len(sessions) > MAX_CORRECTION_SESSIONS,
        }
        qualified = (good_status and metrics.get("historical_original_anchor_qualification") is True
                     and metrics.get("correction_diagnostic_pass") is True
                     and metrics.get("sender_session_pinned") is True
                     and metrics.get("ipc_published_frames", 0) >= 48000
                     and metrics.get("active_generation_usable_measurements", 0) >= 3
                     and details["ipc_failure"] == "none"
                     and all(metrics.get(name) == 0 for name in
                             ("invalid_ingress_anchor_packets", "invalid_sender_reports",
                              "jitter_num_too_late", "jitter_num_drop_on_latency"))
                     and isinstance(sessions, list) and 1 <= len(sessions) <= MAX_CORRECTION_SESSIONS
                     and all(item["state"] == 1 and item["fault"] == 0
                             and item["delivered_frames"] is not None
                             and item["delivered_frames"] >= 48000 for item in safe_sessions))
    return {"status": raw["status"], "metrics": metrics, "reported_media_qualified": bool(qualified), **details}


@dataclass(frozen=True)
class Config:
    receiver_argv: tuple[str, ...]
    sender_argv: tuple[str, ...]
    total_seconds: float = 60.0
    max_attempts: int = 3
    ready_timeout: float = 10.0
    ack_timeout: float = 10.0
    stop_grace: float = 1.0
    require_native_summaries: bool = False
    receiver_scope: str = "direct"
    retire_argv: tuple[str, ...] | None = None
    retire_timeout: float = 8.0
    session_mode: bool = False

    @classmethod
    def from_dict(cls, value: Any) -> "Config":
        allowed = {"receiver_argv", "sender_argv", "total_seconds", "max_attempts",
                   "ready_timeout", "ack_timeout", "stop_grace", "require_native_summaries", "receiver_scope",
                   "retire_argv", "retire_timeout", "session_mode"}
        if not isinstance(value, dict) or set(value) - allowed:
            raise PairError("invalid_config_keys")
        session_mode = value.get("session_mode", False)
        if type(session_mode) is not bool:
            raise PairError("invalid_session_mode")
        duration_flag = "--session-seconds" if session_mode else "--seconds"
        other_duration_flag = "--seconds" if session_mode else "--session-seconds"
        for role in ("receiver", "sender"):
            argv = value.get(role + "_argv")
            if (not isinstance(argv, list) or not 1 <= len(argv) <= 128 or
                any(not isinstance(arg, str) or not arg or len(arg) > 4096 or
                    any(c in arg for c in "\r\n\0") for arg in argv)):
                raise PairError("invalid_config_argv")
            placeholders = {"{sender_session}", "{seconds}"}
            if role == "sender":
                placeholders.add("{clock_epoch}")
            elif value.get("retire_argv") is not None:
                placeholders.add("{run_id}")
            for arg in argv:
                if ("{" in arg or "}" in arg) and arg not in placeholders:
                    raise PairError("non_exact_placeholder")
            if any(arg == other_duration_flag or arg.startswith(other_duration_flag + "=") or
                   arg.startswith(duration_flag + "=") for arg in argv):
                raise PairError("duration_mode_mismatch")
            if session_mode and role == "receiver" and any(
                    arg.split("=", 1)[0] in ("--recover-desktop-ipc", "--clock-pause-after", "--clock-pause-seconds")
                    for arg in argv):
                raise PairError("session_recovery_disabled")
            required = {duration_flag: "{seconds}",
                        "--expect-sender-session" if role == "receiver" else
                        "--sender-session": "{sender_session}"}
            if role == "sender":
                required["--clock-epoch"] = "{clock_epoch}"
                if argv.count("--loopback") != 1:
                    raise PairError("desktop_loopback_required")
            elif value.get("retire_argv") is not None:
                required["--run-id"] = "{run_id}"
            if argv.count("--control-stdin") != 1:
                raise PairError("control_stdin_required")
            for flag, token in required.items():
                if (argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv) or
                    argv[argv.index(flag) + 1] != token or argv.count(token) != 1):
                    raise PairError("required_flag_placeholder")
        for key, low, high in (("total_seconds", 1, 43200 if session_mode else 180), ("ready_timeout", .1, 30),
                               ("ack_timeout", .1, 30), ("stop_grace", .05, 2), ("retire_timeout", .1, 10)):
            setting = value.get(key, getattr(cls, key))
            if (type(setting) not in (int, float) or not math.isfinite(setting) or
                not low <= setting <= high):
                raise PairError("invalid_config_bound")
        attempts = value.get("max_attempts", 1 if session_mode else 3)
        if type(attempts) is not int or not 1 <= attempts <= 3:
            raise PairError("invalid_attempt_bound")
        if session_mode and attempts != 1:
            raise PairError("session_requires_one_attempt")
        if type(value.get("require_native_summaries", False)) is not bool:
            raise PairError("invalid_summary_requirement")
        scope = value.get("receiver_scope", "direct")
        if scope not in ("direct", "remote"):
            raise PairError("invalid_receiver_scope")
        program = value["receiver_argv"][0].replace("\\", "/").rsplit("/", 1)[-1].lower()
        if program in ("ssh", "ssh.exe") and scope != "remote":
            raise PairError("ssh_requires_remote_scope")
        retire = value.get("retire_argv")
        if session_mode and scope == "remote" and retire is None:
            raise PairError("session_remote_retirement_required")
        if retire is not None:
            if (scope != "remote" or not isinstance(retire, list) or not 1 <= len(retire) <= 128 or
                    any(not isinstance(arg, str) or not arg or len(arg) > 4096 or
                        any(c in arg for c in "\r\n\0") for arg in retire)):
                raise PairError("invalid_retire_argv")
            expected = {"{run_id}", "{sender_session}", "{challenge}"}
            if any(retire.count(token) != 1 for token in expected) or any(
                    ("{" in arg or "}" in arg) and arg not in expected for arg in retire):
                raise PairError("invalid_retire_placeholders")
            for flag, token in (("--run-id", "{run_id}"), ("--expect-sender-session", "{sender_session}"),
                                ("--challenge", "{challenge}")):
                if retire.count(flag) != 1 or retire.index(flag) + 1 >= len(retire) or retire[retire.index(flag) + 1] != token:
                    raise PairError("invalid_retire_flag")
        elif "retire_timeout" in value:
            raise PairError("retire_timeout_requires_query")
        result = cls(**{**value, "receiver_argv": tuple(value["receiver_argv"]),
                        "sender_argv": tuple(value["sender_argv"]),
                        "max_attempts": attempts,
                        "retire_argv": tuple(retire) if retire is not None else None})
        # Reserve cooperative cleanup + kill/reap time INSIDE the finite budget.
        if result.total_seconds <= result.stop_grace + .6 + (result.retire_timeout + .5 if retire is not None else 0):
            raise PairError("insufficient_cleanup_budget")
        return result

    def argv(self, role: str, session: str, clock: str | None, seconds: int, run_id: str | None = None) -> list[str]:
        replacements = {"{sender_session}": session, "{seconds}": str(seconds)}
        if clock is not None:
            replacements["{clock_epoch}"] = clock
        if run_id is not None:
            replacements["{run_id}"] = run_id
        return [replacements.get(arg, arg) for arg in getattr(self, role + "_argv")]

    def retirement_argv(self, run_id: str, session: str, challenge: str) -> list[str]:
        replacements = {"{run_id}": run_id, "{sender_session}": session, "{challenge}": challenge}
        return [replacements.get(arg, arg) for arg in (self.retire_argv or ())]


class InstanceLock:
    """OS-held local-file lock; never unlink/reclaim a possibly live lock."""

    def __init__(self, path: Path):
        self.path = path
        self.fd: int | None = None

    def __enter__(self) -> "InstanceLock":
        # Require a private, pre-existing parent. O_NOFOLLOW also protects the
        # final component on platforms that have it; Windows parent ACLs remain
        # the caller's responsibility. Network-filesystem locks are unsupported.
        if not self.path.is_absolute() or not self.path.parent.is_dir():
            raise PairError("invalid_lock_path")
        if self.path.is_symlink():
            raise PairError("unsafe_lock_file")
        try:
            if os.name != "nt":
                parent_info = self.path.parent.lstat()
                if (not stat.S_ISDIR(parent_info.st_mode) or parent_info.st_uid != os.getuid() or
                    stat.S_IMODE(parent_info.st_mode) & 0o077):
                    raise PairError("unsafe_lock_directory")
            self.fd = os.open(self.path, os.O_RDWR | os.O_CREAT |
                              getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_NONBLOCK", 0), 0o600)
            info = os.fstat(self.fd)
            if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
                raise PairError("unsafe_lock_file")
            if os.name != "nt" and (info.st_uid != os.getuid() or stat.S_IMODE(info.st_mode) & 0o077):
                raise PairError("unsafe_lock_file")
            if os.name == "nt":
                import msvcrt
                # Windows byte-range locking permits a region past EOF. Do not
                # write, truncate, or replace another coordinator's lock file.
                os.lseek(self.fd, 0, os.SEEK_SET)
                msvcrt.locking(self.fd, msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(self.fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            # Reject a replacement between opening and acquiring. Keeping the
            # containing directory private prevents untrusted later replacement.
            linked = self.path.lstat()
            if (not stat.S_ISREG(linked.st_mode) or linked.st_nlink != 1 or
                (linked.st_dev, linked.st_ino) != (info.st_dev, info.st_ino)):
                raise PairError("lock_identity_changed")
        except (OSError, PairError):
            if self.fd is not None:
                os.close(self.fd)
                self.fd = None
            raise PairError("lock_unavailable") from None
        return self

    def __exit__(self, *_: Any) -> None:
        if self.fd is not None:
            # Closing releases this handle's OS lock, not someone else's file.
            os.close(self.fd)
            self.fd = None


class Child:
    def __init__(self, role: str, argv: list[str]):
        self.role = role
        self.events: queue.Queue[tuple[str, Any]] = queue.Queue(MAX_PENDING)
        self.writes: queue.Queue[bytes | None] = queue.Queue(2)
        self.failure: str | None = None
        self.output_failure: str | None = None
        self.native_summary: dict[str, Any] | None = None
        self.native_summary_error: str | None = None
        self.native_summary_count = 0
        self.discarded_stdout_lines = 0
        self.stdout_done = threading.Event()
        self.stderr_done = threading.Event()
        self.output_lock = threading.Lock()
        self.output_bytes = 0
        self.last_keepalive = -math.inf
        self.threads: list[threading.Thread] = []
        options: dict[str, Any] = {}
        if os.name == "nt":
            options["creationflags"] = subprocess.CREATE_NO_WINDOW
        try:
            self.process = subprocess.Popen(argv, stdin=subprocess.PIPE,
                                            stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, bufsize=0,
                                            shell=False, **options)
        except OSError:
            raise PairError(role + "_spawn_failed") from None
        for target in (self._read_stdout, self._drain_stderr, self._write_stdin):
            thread = threading.Thread(target=target, daemon=True,
                                      name="avsync-control-" + role)
            self.threads.append(thread)
            thread.start()

    def _fail(self, reason: str) -> None:
        # Assignment of a reference is atomic under the supported CPython
        # runtime. All reasons here are fixed codes, never child output.
        # A recoverable broken stdin pipe must not hide a later stdout/stderr
        # corruption fault from another reader thread during retirement.
        if reason in OUTPUT_FAILURES and self.output_failure is None:
            self.output_failure = reason
        if self.failure is None:
            self.failure = reason

    def _emit(self, kind: str, value: Any = None) -> None:
        try:
            self.events.put_nowait((kind, value))
        except queue.Full:
            self._fail("control_output_overflow")

    def _count_output(self, size: int) -> bool:
        with self.output_lock:
            self.output_bytes += size
            if self.output_bytes > MAX_CHILD_OUTPUT:
                self._fail("output_total_exceeded")
                return False
        return True

    @staticmethod
    def _line_limit(line: bytes | bytearray) -> int:
        if line.startswith(b"AVSYNC_CONTROL"):
            return MAX_CONTROL
        return MAX_NATIVE_SUMMARY if line.lstrip().startswith(b"{") else MAX_LINE

    def _capture_summary(self, line: bytes) -> None:
        self.native_summary_count += 1
        if self.native_summary_count != 1:
            self.native_summary_error = "duplicate_native_summary"
            return
        try:
            self.native_summary = parse_native_summary(line, self.role)
        except PairError as error:
            self.native_summary_error = str(error)

    def _read_stdout(self) -> None:
        pending = bytearray()
        try:
            while True:
                chunk = self.process.stdout.read(4096)
                if not chunk:
                    if pending:
                        self._fail("truncated_stdout_line")
                    self._emit("error", "stdout_eof")
                    return
                if not self._count_output(len(chunk)):
                    return
                pending.extend(chunk)
                while b"\n" in pending:
                    end = pending.index(b"\n")
                    if end > self._line_limit(pending[:end]):
                        self._fail("output_line_too_long")
                        return
                    line = bytes(pending[:end])
                    del pending[:end + 1]
                    # Binary pipes retain CRLF from some native Windows apps.
                    line = line.removesuffix(b"\r")
                    if line.startswith(b"AVSYNC_CONTROL"):
                        self._emit("control", line)
                    elif line.lstrip().startswith(b"{"):
                        self._capture_summary(line)
                    else:
                        self.discarded_stdout_lines += 1
                if len(pending) > self._line_limit(pending):
                    self._fail("output_line_too_long")
                    return
        except (OSError, ValueError):
            self._fail("stdout_read_failed")
        finally:
            self.stdout_done.set()

    def _drain_stderr(self) -> None:
        try:
            while chunk := self.process.stderr.read(4096):
                if not self._count_output(len(chunk)):
                    return
        except (OSError, ValueError):
            self._fail("stderr_read_failed")
        finally:
            self.stderr_done.set()

    def _write_stdin(self) -> None:
        try:
            while True:
                data = self.writes.get()
                if data is None:
                    return
                view = memoryview(data)
                while view:
                    written = self.process.stdin.write(view)
                    if not written:
                        raise OSError("short_pipe_write")
                    view = view[written:]
        except (OSError, ValueError):
            self._fail("stdin_write_failed")

    def send(self, data: bytes) -> None:
        try:
            self.writes.put_nowait(data)
        except queue.Full:
            self._fail("stdin_backpressure")

    def keepalive(self, now: float) -> None:
        if now - self.last_keepalive >= 1.0:
            self.send(KEEPALIVE)
            self.last_keepalive = now

    def close_pipes(self) -> None:
        try:
            self.writes.put_nowait(None)
        except queue.Full:
            pass
        for thread in self.threads:
            thread.join(timeout=.05)
        if not self.stdout_done.is_set() or not self.stderr_done.is_set():
            self._fail("output_not_drained")
        for pipe in (self.process.stdin, self.process.stdout, self.process.stderr):
            try:
                pipe.close()
            except OSError:
                pass


@dataclass
class Fault:
    attempt: int
    phase: str
    reason: str


@dataclass
class Result:
    schema: int = 1
    status: str = "failed"
    evidence: str = "process_agreement_only"
    attempts: int = 0
    acknowledged_pairs: int = 0
    faults: list[Fault] = field(default_factory=list)
    forced_local_kills: int = 0
    cleanup_failures: int = 0
    stop_failures: int = 0
    elapsed_seconds: float = 0.0
    native_diagnostics: list[dict[str, Any]] = field(default_factory=list)
    native_summary_requirement_met: bool | None = None
    reported_media_qualified: bool = False
    media_verified: bool = False
    remote_retirement_unverified: bool = False
    remote_retirement_checks: list[dict[str, Any]] = field(default_factory=list)

    @property
    def exit_code(self) -> int:
        if self.status == "control_completed" and not self.faults:
            return 0
        return 3 if self.status == "control_completed_with_recovery" else 1


class Supervisor:
    def __init__(self, config: Config, lock_path: Path):
        self.config = config
        self.lock_path = lock_path
        self.result = Result()
        self.children: list[Child] = []
        self.sessions: set[str] = set()
        self.clocks: set[str] = set()
        self.proof_tokens: set[str] = set()
        self.pending_retirement: tuple[str, str] | None = None
        self.terminal_cleanup = False
        self.terminal_protocol = False

    def _new_proof_token(self) -> str:
        for _ in range(16):
            token = secrets.token_hex(16)
            if token not in self.proof_tokens:
                self.proof_tokens.add(token)
                return token
        raise PairError("retirement_entropy_failed")

    def _new_session(self) -> str:
        for _ in range(16):
            token = str(secrets.randbits(64))
            if token != "0" and token not in self.sessions:
                self.sessions.add(token)
                return token
        raise PairError("session_entropy_failed")

    def _poll(self, expected: Child | None = None,
              event: str | None = None) -> dict[str, Any] | None:
        answer = None
        for child in self.children:
            if child.failure:
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
                raise PairError(child.role + "_" + child.failure)
            if child.process.poll() is not None:
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
                raise PairError(child.role + "_exited")
            child.keepalive(time.monotonic())
            # A bounded queue and bounded scan ensure the other child, lease,
            # and deadline cannot be starved by a control-line flood.
            for _ in range(MAX_PENDING):
                try:
                    kind, value = child.events.get_nowait()
                except queue.Empty:
                    break
                if kind == "error":
                    if child.role == "receiver" and self.config.receiver_scope == "remote":
                        self.result.remote_retirement_unverified = True
                    raise PairError(child.role + "_" + value)
                message = parse_control(value)
                if child is not expected or message["event"] != event or answer is not None:
                    raise PairError("unexpected_control_event")
                answer = message
        return answer

    def _wait_handshake(self, child: Child, event: str, session: str,
                        clock: str | None, timeout: float, deadline: float) -> str:
        phase_deadline = min(deadline, time.monotonic() + timeout)
        while time.monotonic() < phase_deadline:
            message = self._poll(child, event)
            if message is not None:
                if message["sender_session"] != session:
                    raise PairError("sender_session_mismatch")
                token = message["clock_epoch"]
                if clock is not None and token != clock:
                    raise PairError("clock_epoch_mismatch")
                if clock is None:
                    if token in self.clocks:
                        raise PairError("reused_provider_epoch")
                    self.clocks.add(token)
                return token
            time.sleep(min(.02, max(0, phase_deadline - time.monotonic())))
        raise PairError("overall_deadline" if time.monotonic() >= deadline else
                        event + "_timeout")

    def _stop_pair(self, budget_end: float) -> None:
        children, self.children = self.children, []
        retirement, self.pending_retirement = self.pending_retirement, None
        proof_enabled = self.config.retire_argv is not None
        asked_to_stop: list[Child] = []
        for child in children:
            if child.process.poll() is None:
                child.send(STOP)
                asked_to_stop.append(child)
            else:
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
                if self.result.status in ("control_completed", "control_completed_with_recovery"):
                    self.result.stop_failures += 1
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", child.role + "_exited_before_stop"))
                    if not proof_enabled or child.role != "receiver":
                        self.terminal_cleanup = True
        grace_end = min(budget_end - .3, time.monotonic() + self.config.stop_grace)
        while time.monotonic() < grace_end and any(c.process.poll() is None for c in children):
            time.sleep(.01)
        for child in children:
            if child.process.poll() is None:
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
                try:
                    child.process.kill()  # Exact Popen-owned local child only.
                    self.result.forced_local_kills += 1
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", child.role + "_forced_local_stop"))
                    if not proof_enabled or child.role != "receiver":
                        self.terminal_cleanup = True
                except OSError:
                    pass
        for child in children:
            try:
                child.process.wait(timeout=max(.01, min(.3, budget_end - time.monotonic())))
            except subprocess.TimeoutExpired:
                self.result.cleanup_failures += 1
                self.terminal_cleanup = True
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
            if child in asked_to_stop and child.process.returncode not in (None, 0):
                self.result.stop_failures += 1
                self.result.faults.append(Fault(self.result.attempts, "cleanup", child.role + "_nonzero_stop_exit"))
                if child.role == "receiver" and self.config.receiver_scope == "remote":
                    self.result.remote_retirement_unverified = True
                if not proof_enabled or child.role != "receiver":
                    self.terminal_cleanup = True
            child.close_pipes()
            # Final stdout is produced during STOP, after the last running poll.
            # EOF is expected here; other reader faults must not disappear.
            if child.output_failure is not None:
                self.terminal_cleanup = True
                reason = child.role + "_" + child.output_failure
                if not any(f.attempt == self.result.attempts and f.reason == reason for f in self.result.faults):
                    self.result.stop_failures += 1
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", reason))
            for _ in range(MAX_PENDING):
                try:
                    kind, _ = child.events.get_nowait()
                except queue.Empty:
                    break
                if kind == "control":
                    self.terminal_cleanup = True
                    self.result.stop_failures += 1
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", "unexpected_final_control_event"))
                    break
            summary = child.native_summary
            valid = summary is not None and child.native_summary_count == 1 and child.native_summary_error is None
            reason = child.native_summary_error or ("native_summary_missing" if summary is None else
                      "reported_qualified" if summary["reported_media_qualified"] else "reported_unqualified")
            if child.failure:
                valid, reason = False, "native_output_error"
            self.result.native_diagnostics.append({
                "attempt": self.result.attempts, "role": child.role,
                "summary_count": child.native_summary_count, "valid": valid, "reason": reason,
                "status": summary["status"] if summary else None,
                "metrics": summary["metrics"] if summary else {},
                **({name: summary[name] for name in
                    ("ipc_failure", "correction_sessions", "correction_session_count", "correction_sessions_truncated")}
                   if summary and child.role == "receiver" else {}),
                "reported_media_qualified": valid and bool(summary["reported_media_qualified"]),
                "discarded_stdout_lines": child.discarded_stdout_lines,
                "output_bytes": child.output_bytes,
            })
        if retirement is not None:
            run_id, session = retirement
            self.result.remote_retirement_unverified = True
            try:
                challenge = self._new_proof_token()
                proof = verify_retirement(self.config.retirement_argv(run_id, session, challenge),
                    run_id, session, challenge, min(self.config.retire_timeout, budget_end - time.monotonic() - .5))
            except PairError:
                proof = {"verified": False, "proof": None, "reason": "retirement_entropy_failed",
                         "forced_local_kill": False}
            self.result.remote_retirement_checks.append({"attempt": self.result.attempts, **proof})
            if proof["verified"]:
                self.result.remote_retirement_unverified = False
            else:
                self.result.faults.append(Fault(self.result.attempts, "retirement", proof["reason"]))
                self.terminal_cleanup = True
        if proof_enabled and (self.terminal_cleanup or self.terminal_protocol):
            self.result.status = "failed"
        elif proof_enabled and self.result.status == "control_completed" and self.result.faults:
            self.result.status = "control_completed_with_recovery"

    def run(self) -> Result:
        started = time.monotonic()
        final_deadline = started + self.config.total_seconds
        # Stop media/control early enough for cleanup within this finite run.
        active_deadline = final_deadline - self.config.stop_grace - .6 - (
            self.config.retire_timeout + .5 if self.config.retire_argv is not None else 0)
        phase = "lock"
        try:
            with InstanceLock(self.lock_path):
                for attempt in range(1, self.config.max_attempts + 1):
                    if time.monotonic() >= active_deadline:
                        self.result.faults.append(Fault(attempt, "start", "overall_deadline"))
                        break
                    self.result.attempts = attempt
                    phase = "receiver_start"
                    try:
                        session = self._new_session()
                        run_id = self._new_proof_token() if self.config.retire_argv is not None else None
                        if run_id is not None:
                            # Before starting SSH: even a pre-READY uncertainty
                            # needs an independent irreversible cancellation fence.
                            self.pending_retirement = (run_id, session)
                            self.result.remote_retirement_unverified = True
                        seconds = max(1, min(43200 if self.config.session_mode else 180,
                                             math.ceil(final_deadline - time.monotonic())))
                        receiver = Child("receiver", self.config.argv("receiver", session, None, seconds, run_id))
                        self.children.append(receiver)
                        clock = self._wait_handshake(receiver, "receiver_ready", session, None,
                                                     self.config.ready_timeout, active_deadline)
                        phase = "sender_start"
                        sender = Child("sender", self.config.argv("sender", session, clock, seconds))
                        self.children.append(sender)
                        self._wait_handshake(sender, "sender_started", session, clock,
                                             self.config.ack_timeout, active_deadline)
                        self.result.acknowledged_pairs += 1
                        phase = "running"
                        while time.monotonic() < active_deadline:
                            self._poll()
                            time.sleep(min(.02, max(0, active_deadline - time.monotonic())))
                        self._poll()  # Do not discard a fault queued at the deadline edge.
                        self.result.status = ("control_completed_with_recovery" if self.result.faults
                                              else "control_completed")
                        break
                    except PairError as error:
                        self.result.faults.append(Fault(attempt, phase, str(error)))
                        if self.config.retire_argv is not None and str(error) not in {
                            "receiver_spawn_failed", "sender_spawn_failed", "receiver_exited", "sender_exited",
                            "receiver_stdout_eof", "sender_stdout_eof", "receiver_stdin_write_failed",
                            "receiver_stdin_backpressure", "receiver_ready_timeout", "sender_started_timeout",
                        }:
                            # Proof resolves process containment only. It cannot
                            # excuse invalid source identity or protocol corruption.
                            self.terminal_protocol = True
                    finally:
                        self._stop_pair(final_deadline)
                    # A killed SSH child is not proof that a remote receiver has
                    # retired. Never launch another pair after ambiguous cleanup.
                    unsafe = (self.terminal_cleanup or self.terminal_protocol) if self.config.retire_argv is not None else (
                        self.result.cleanup_failures or self.result.forced_local_kills or self.result.stop_failures)
                    if unsafe or self.result.remote_retirement_unverified:
                        break
                unsafe = (self.terminal_cleanup or self.terminal_protocol) if self.config.retire_argv is not None else (
                    self.result.forced_local_kills or self.result.cleanup_failures or self.result.stop_failures)
                if unsafe:
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", "unclean_child_stop"))
                    self.result.status = "failed"
                if self.result.remote_retirement_unverified:
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", "remote_retirement_unverified"))
                    self.result.status = "failed"
        except PairError as error:
            self.result.faults.append(Fault(self.result.attempts, phase, str(error)))
        except KeyboardInterrupt:
            self.result.faults.append(Fault(self.result.attempts, phase, "user_interrupted"))
            self.result.status = "failed"
        finally:
            self._stop_pair(final_deadline)
            self.result.elapsed_seconds = round(time.monotonic() - started, 3)
            diagnostics = self.result.native_diagnostics
            complete = (len(diagnostics) == 2 * self.result.attempts and self.result.attempts > 0
                        and all(item["valid"] for item in diagnostics))
            if self.config.require_native_summaries:
                self.result.native_summary_requirement_met = complete
            self.result.reported_media_qualified = (complete
                and self.result.status in ("control_completed", "control_completed_with_recovery")
                and all(item["reported_media_qualified"] for item in diagnostics))
        return self.result


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, required=True,
                        help="Private trusted JSON argv configuration; no shell commands")
    parser.add_argument("--lock-file", type=Path, required=True,
                        help="Absolute fixed lock path in a private local directory")
    args = parser.parse_args()
    try:
        if args.config.stat().st_size > 65536:
            raise PairError("config_too_large")
        with args.config.open(encoding="utf-8") as handle:
            raw = json.load(handle, object_pairs_hook=_unique_object)
        result = Supervisor(Config.from_dict(raw), args.lock_file).run()
    except (OSError, ValueError, PairError):
        print(json.dumps({"schema": 1, "status": "invalid_configuration"}))
        return 2
    print(json.dumps(asdict(result), sort_keys=True))
    return result.exit_code


if __name__ == "__main__":
    raise SystemExit(main())
