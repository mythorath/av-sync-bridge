#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Finite, desktop-only process agreement fixture. Not an audio health monitor."""

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


PREFIX = b"AVSYNC_CONTROL "
MAX_LINE = 4096
MAX_CONTROL = 1024
MAX_PENDING = 64
UINT64_MAX = (1 << 64) - 1
KEEPALIVE = b"AVSYNC_KEEPALIVE\n"
STOP = b"AVSYNC_STOP\n"


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


@dataclass(frozen=True)
class Config:
    receiver_argv: tuple[str, ...]
    sender_argv: tuple[str, ...]
    total_seconds: float = 60.0
    max_attempts: int = 3
    ready_timeout: float = 10.0
    ack_timeout: float = 10.0
    stop_grace: float = 1.0

    @classmethod
    def from_dict(cls, value: Any) -> "Config":
        allowed = {"receiver_argv", "sender_argv", "total_seconds", "max_attempts",
                   "ready_timeout", "ack_timeout", "stop_grace"}
        if not isinstance(value, dict) or set(value) - allowed:
            raise PairError("invalid_config_keys")
        for role in ("receiver", "sender"):
            argv = value.get(role + "_argv")
            if (not isinstance(argv, list) or not 1 <= len(argv) <= 128 or
                any(not isinstance(arg, str) or not arg or len(arg) > 4096 or
                    any(c in arg for c in "\r\n\0") for arg in argv)):
                raise PairError("invalid_config_argv")
            placeholders = {"{sender_session}", "{seconds}"}
            if role == "sender":
                placeholders.add("{clock_epoch}")
            for arg in argv:
                if ("{" in arg or "}" in arg) and arg not in placeholders:
                    raise PairError("non_exact_placeholder")
            required = {"--seconds": "{seconds}",
                        "--expect-sender-session" if role == "receiver" else
                        "--sender-session": "{sender_session}"}
            if role == "sender":
                required["--clock-epoch"] = "{clock_epoch}"
                if argv.count("--loopback") != 1:
                    raise PairError("desktop_loopback_required")
            if argv.count("--control-stdin") != 1:
                raise PairError("control_stdin_required")
            for flag, token in required.items():
                if (argv.count(flag) != 1 or argv.index(flag) + 1 >= len(argv) or
                    argv[argv.index(flag) + 1] != token):
                    raise PairError("required_flag_placeholder")
        for key, low, high in (("total_seconds", 1, 180), ("ready_timeout", .1, 30),
                               ("ack_timeout", .1, 30), ("stop_grace", .05, 2)):
            setting = value.get(key, getattr(cls, key))
            if (type(setting) not in (int, float) or not math.isfinite(setting) or
                not low <= setting <= high):
                raise PairError("invalid_config_bound")
        attempts = value.get("max_attempts", 3)
        if type(attempts) is not int or not 1 <= attempts <= 3:
            raise PairError("invalid_attempt_bound")
        result = cls(**{**value, "receiver_argv": tuple(value["receiver_argv"]),
                        "sender_argv": tuple(value["sender_argv"])})
        # Reserve cooperative cleanup + kill/reap time INSIDE the finite budget.
        if result.total_seconds <= result.stop_grace + .6:
            raise PairError("insufficient_cleanup_budget")
        return result

    def argv(self, role: str, session: str, clock: str | None, seconds: int) -> list[str]:
        replacements = {"{sender_session}": session, "{seconds}": str(seconds)}
        if clock is not None:
            replacements["{clock_epoch}"] = clock
        return [replacements.get(arg, arg) for arg in getattr(self, role + "_argv")]


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
        if self.failure is None:
            self.failure = reason

    def _emit(self, kind: str, value: Any = None) -> None:
        try:
            self.events.put_nowait((kind, value))
        except queue.Full:
            self._fail("control_output_overflow")

    def _read_stdout(self) -> None:
        pending = bytearray()
        try:
            while True:
                chunk = self.process.stdout.read(4096)
                if not chunk:
                    self._emit("error", "stdout_eof")
                    return
                pending.extend(chunk)
                while b"\n" in pending:
                    end = pending.index(b"\n")
                    if end > MAX_LINE:
                        self._fail("output_line_too_long")
                        return
                    line = bytes(pending[:end])
                    del pending[:end + 1]
                    # Binary pipes retain CRLF from some native Windows apps.
                    line = line.removesuffix(b"\r")
                    if line.startswith(b"AVSYNC_CONTROL"):
                        self._emit("control", line)
                if len(pending) > MAX_LINE:
                    self._fail("output_line_too_long")
                    return
        except (OSError, ValueError):
            self._fail("stdout_read_failed")

    def _drain_stderr(self) -> None:
        try:
            while self.process.stderr.read(4096):
                pass  # Deliberately no unbounded logs or trusted handshakes here.
        except (OSError, ValueError):
            self._fail("stderr_read_failed")

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
                raise PairError(child.role + "_" + child.failure)
            if child.process.poll() is not None:
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
        asked_to_stop: list[Child] = []
        for child in children:
            if child.process.poll() is None:
                child.send(STOP)
                asked_to_stop.append(child)
            elif self.result.status in ("control_completed", "control_completed_with_recovery"):
                self.result.stop_failures += 1
                self.result.faults.append(Fault(self.result.attempts, "cleanup", child.role + "_exited_before_stop"))
        grace_end = min(budget_end - .3, time.monotonic() + self.config.stop_grace)
        while time.monotonic() < grace_end and any(c.process.poll() is None for c in children):
            time.sleep(.01)
        for child in children:
            if child.process.poll() is None:
                try:
                    child.process.kill()  # Exact Popen-owned local child only.
                    self.result.forced_local_kills += 1
                except OSError:
                    pass
        for child in children:
            try:
                child.process.wait(timeout=max(.01, min(.3, budget_end - time.monotonic())))
            except subprocess.TimeoutExpired:
                self.result.cleanup_failures += 1
            if child in asked_to_stop and child.process.returncode not in (None, 0):
                self.result.stop_failures += 1
                self.result.faults.append(Fault(self.result.attempts, "cleanup", child.role + "_nonzero_stop_exit"))
            child.close_pipes()

    def run(self) -> Result:
        started = time.monotonic()
        final_deadline = started + self.config.total_seconds
        # Stop media/control early enough for cleanup within this finite run.
        active_deadline = final_deadline - self.config.stop_grace - .6
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
                        seconds = max(1, min(180, math.ceil(final_deadline - time.monotonic())))
                        receiver = Child("receiver", self.config.argv("receiver", session, None, seconds))
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
                    finally:
                        self._stop_pair(final_deadline)
                    # A killed SSH child is not proof that a remote receiver has
                    # retired. Never launch another pair after ambiguous cleanup.
                    if (self.result.cleanup_failures or self.result.forced_local_kills or
                        self.result.stop_failures):
                        break
                if (self.result.forced_local_kills or self.result.cleanup_failures or
                    self.result.stop_failures):
                    self.result.faults.append(Fault(self.result.attempts, "cleanup", "unclean_child_stop"))
                    self.result.status = "failed"
        except PairError as error:
            self.result.faults.append(Fault(self.result.attempts, phase, str(error)))
        except KeyboardInterrupt:
            self.result.faults.append(Fault(self.result.attempts, phase, "user_interrupted"))
            self.result.status = "failed"
        finally:
            self._stop_pair(final_deadline)
            self.result.elapsed_seconds = round(time.monotonic() - started, 3)
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
