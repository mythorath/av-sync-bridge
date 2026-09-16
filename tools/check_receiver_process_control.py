#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit Linux loopback control/empty-IPC test. No capture, PCM or OBS."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import queue
import secrets
import signal
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time


PREFIX = b"AVSYNC_CONTROL "
MAX_LINE = 65536  # Native final diagnostic objects can exceed 4096 bytes.
MAX_OUTPUT = 4 * 1024 * 1024
KEEPALIVE = b"AVSYNC_KEEPALIVE\n"
STOP = b"AVSYNC_STOP\n"


class CheckFailure(Exception):
    """Only fixed, non-sensitive reason codes leave this program."""


def require(condition: bool, reason: str) -> None:
    if not condition:
        raise CheckFailure(reason)


def unique_object(pairs):
    result = {}
    for key, value in pairs:
        require(key not in result, "duplicate_json_key")
        result[key] = value
    return result


def decode(line: bytes):
    try:
        return json.loads(line.decode("ascii"), object_pairs_hook=unique_object)
    except (ValueError, UnicodeError, RecursionError):
        raise CheckFailure("invalid_native_json") from None


def decimal_identity(value) -> bool:
    return (isinstance(value, str) and value.isascii() and value.isdecimal()
            and not value.startswith("0") and len(value) <= 20
            and 0 < int(value) <= (1 << 64) - 1)


def fresh_session(seen: set[str]) -> str:
    for _ in range(16):
        value = str(secrets.randbits(64))
        if value != "0" and value not in seen:
            seen.add(value)
            return value
    raise CheckFailure("identity_generation_failed")


def unused_ports() -> list[int]:
    # Reserve all three together to avoid duplicates, then release immediately
    # before launch. A competing binder still fails native startup, never causes
    # this tool to kill or reconfigure the owner of a conflicting port.
    sockets = []
    try:
        for _ in range(3):
            candidate = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            sockets.append(candidate)
            candidate.bind(("127.0.0.1", 0))
        ports = [item.getsockname()[1] for item in sockets]
        require(len(set(ports)) == 3 and min(ports) >= 1024, "port_selection_failed")
        return ports
    finally:
        for item in sockets:
            item.close()


class Child:
    def __init__(self, argv: list[str], prebuffered: bytes | None):
        self.events: queue.Queue[tuple[str, bytes]] = queue.Queue(maxsize=64)
        self.reader_failed = threading.Event()
        self.streams_closed = 0
        self.ready = None
        self.summary = None
        self.legacy_ready = 0
        self.next_keepalive = time.monotonic()
        self.write_fd = None
        self.process = None
        reader, writer = os.pipe()
        try:
            if prebuffered is not None:
                if prebuffered:
                    require(os.write(writer, prebuffered) == len(prebuffered), "prefill_failed")
                os.close(writer)
                writer = None
            self.process = subprocess.Popen(argv, stdin=reader, stdout=subprocess.PIPE,
                                            stderr=subprocess.PIPE, close_fds=True,
                                            shell=False)
            self.write_fd = writer
            writer = None
            if self.write_fd is not None:
                os.set_blocking(self.write_fd, False)
        except BaseException:
            if self.process is not None:
                if self.process.poll() is None:
                    self.process.kill()
                self.process.wait(timeout=2)
                for stream in (self.process.stdout, self.process.stderr):
                    stream.close()
            if self.write_fd is not None:
                os.close(self.write_fd)
                self.write_fd = None
            raise
        finally:
            os.close(reader)
            if writer is not None:
                os.close(writer)
        self.threads = []
        for label, stream in (("stdout", self.process.stdout), ("stderr", self.process.stderr)):
            thread = threading.Thread(target=self.read_stream, args=(label, stream), daemon=True)
            self.threads.append(thread)
            thread.start()

    def read_stream(self, label, stream):
        total = 0
        try:
            while True:
                line = stream.readline(MAX_LINE + 1)
                if not line:
                    self.events.put_nowait(("eof", b""))
                    return
                total += len(line)
                if len(line) > MAX_LINE or total > MAX_OUTPUT:
                    self.reader_failed.set()
                    return
                if label == "stdout":
                    self.events.put_nowait((label, line.rstrip(b"\r\n")))
                # Stderr is deliberately discarded, never copied into reports.
        except (OSError, ValueError, queue.Full):
            self.reader_failed.set()

    def send(self, data: bytes):
        require(self.write_fd is not None, "control_pipe_closed")
        try:
            require(os.write(self.write_fd, data) == len(data), "control_partial_write")
        except OSError:
            raise CheckFailure("control_write_failed") from None

    def pump(self, renew: bool):
        require(not self.reader_failed.is_set(), "native_output_limit_or_read_error")
        for _ in range(64):
            try:
                label, line = self.events.get_nowait()
            except queue.Empty:
                break
            if label == "eof":
                self.streams_closed += 1
            elif line.startswith(PREFIX):
                require(self.ready is None, "duplicate_native_ready")
                message = decode(line[len(PREFIX):])
                require(isinstance(message, dict) and set(message) == {
                    "schema", "event", "clock_epoch", "sender_session"
                }, "native_ready_schema")
                require(type(message["schema"]) is int and message["schema"] == 1
                        and message["event"] == "receiver_ready"
                        and decimal_identity(message["clock_epoch"])
                        and decimal_identity(message["sender_session"]), "native_ready_identity")
                self.ready = message
            elif line.startswith(b"AVSYNC_NETWORK_READY "):
                self.legacy_ready += 1
            elif line.startswith(b"{"):
                require(self.summary is None, "duplicate_native_summary")
                self.summary = decode(line)
                require(isinstance(self.summary, dict), "native_summary_schema")
        if renew and self.process.poll() is None and time.monotonic() >= self.next_keepalive:
            self.send(KEEPALIVE)
            self.next_keepalive = time.monotonic() + 0.5

    def wait_ready(self, session: str, deadline: float):
        while time.monotonic() < deadline:
            self.pump(renew=True)
            if self.ready is not None:
                require(self.ready["sender_session"] == session and self.legacy_ready == 1,
                        "ready_agreement_mismatch")
                require(self.process.poll() is None, "receiver_exited_at_ready")
                return self.ready["clock_epoch"]
            require(self.process.poll() is None, "receiver_exited_before_ready")
            time.sleep(0.01)
        raise CheckFailure("receiver_ready_timeout")

    def wait_exit(self, deadline: float):
        while time.monotonic() < deadline:
            self.pump(renew=False)
            code = self.process.poll()
            if code is not None and self.streams_closed == 2:
                return code
            time.sleep(0.01)
        raise CheckFailure("receiver_exit_timeout")

    def close(self):
        if self.write_fd is not None:
            os.close(self.write_fd)
            self.write_fd = None
        if self.process.poll() is None:
            self.process.kill()  # Exact owned Popen child only.
        try:
            self.process.wait(timeout=2)
        except subprocess.TimeoutExpired:
            raise CheckFailure("owned_child_reap_failed") from None
        for thread in self.threads:
            thread.join(timeout=0.5)
        # Do not block closing a buffered stream while its reader thread owns
        # the I/O lock (for example, an unexpected descendant retained a pipe).
        require(all(not thread.is_alive() for thread in self.threads), "native_reader_cleanup_failed")
        for stream in (self.process.stdout, self.process.stderr):
            stream.close()


def mapping_identity(path: Path):
    info = path.lstat()
    require(stat.S_ISREG(info.st_mode) and stat.S_IMODE(info.st_mode) == 0o600
            and info.st_uid == os.geteuid() and info.st_nlink == 1
            and 32 <= info.st_size <= 16 * 1024 * 1024, "unexpected_private_mapping")
    with path.open("rb") as stream:
        prefix = stream.read(32)
    # Only immutable prefix fields are inspected; no native mutex/header mutation.
    require(int.from_bytes(prefix[0:8], sys.byteorder) == 0x415653594E433031
            and int.from_bytes(prefix[8:12], sys.byteorder) == 2
            and int.from_bytes(prefix[16:24], sys.byteorder) == info.st_size,
            "expected_protocol_v2_mapping")
    generation = int.from_bytes(prefix[24:32], sys.byteorder)
    require(generation != 0, "invalid_mapping_generation")
    return info.st_dev, info.st_ino, generation


def fingerprint(path: Path):
    identity = mapping_identity(path)
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        while block := stream.read(65536):
            digest.update(block)
    return identity, path.stat().st_size, digest.digest()


def final_status(child: Child, status: str, code: int, deadline: float, *, early=False):
    require(child.wait_exit(deadline) == code, "unexpected_exit_code")
    summary = child.summary
    require(isinstance(summary, dict) and summary.get("schema") == 1
            and summary.get("status") == status
            and summary.get("capture_timing_verified") is False, "unexpected_final_summary")
    if early:
        require(child.ready is None and child.legacy_ready == 0, "cancelled_child_announced_ready")
    else:
        require(summary.get("control_state") == status
                and summary.get("sender_session_pinned") is True
                and summary.get("packets_accepted") == 0
                and summary.get("ipc_published_frames") == 0,
                "unexpected_media_or_control_result")


def check(executable: Path, total_seconds: float):
    started = time.monotonic()
    overall = started + total_seconds
    deadline = lambda: min(overall - 3, time.monotonic() + 8)
    sessions: set[str] = set()
    clocks: set[str] = set()
    owned: list[Child] = []
    report = {"schema": 1, "passed": False, "loopback_only": True,
              "evidence": "native_process_control_and_empty_ipc_replacement",
              "pcm_injected": False, "capture_verified": False, "obs_verified": False,
              "checks_completed": 0, "children_started": 0}
    with tempfile.TemporaryDirectory(prefix="avsync-loopback-control-") as temporary:
        runtime = Path(temporary)
        os.chmod(runtime, 0o700)
        path = runtime / "desktop.ipc"

        def launch(prebuffered=None):
            require(time.monotonic() < overall - 5, "overall_deadline")
            session = fresh_session(sessions)
            clock_port, rtp_port, rtcp_port = unused_ports()
            argv = [str(executable), "--bind", "127.0.0.1", "--peer", "127.0.0.1",
                    "--clock-port", str(clock_port), "--rtp-port", str(rtp_port),
                    "--rtcp-port", str(rtcp_port), "--seconds", "20",
                    "--expect-anchors", "--expect-sender-session", session,
                    "--control-stdin", "--desktop-ipc", str(path), "--replace-desktop-ipc"]
            child = Child(argv, prebuffered)
            owned.append(child)
            report["children_started"] += 1
            return child, session

        def ready(child, session):
            clock = child.wait_ready(session, deadline())
            require(clock not in clocks, "provider_identity_reused")
            clocks.add(clock)
            return mapping_identity(path)

        try:
            first, session = launch()
            first_identity = ready(first, session)
            first.send(STOP)
            final_status(first, "control_stopped", 0, deadline())
            first.close()
            baseline = fingerprint(path)
            report["checks_completed"] += 1

            # Cancellation bytes/EOF already exist BEFORE native process starts.
            for prebuffered, status, code in ((STOP, "control_stopped", 0),
                                              (b"", "control_eof", 1)):
                cancelled, _ = launch(prebuffered)
                final_status(cancelled, status, code, deadline(), early=True)
                cancelled.close()
                require(fingerprint(path) == baseline, "cancelled_start_mutated_predecessor")
                report["checks_completed"] += 1

            crashed, session = launch()
            crash_identity = ready(crashed, session)
            require(crash_identity[0:2] != first_identity[0:2]
                    and crash_identity[2] != first_identity[2], "normal_restart_reused_mapping")
            report["checks_completed"] += 1
            crashed.process.kill()
            require(crashed.wait_exit(deadline()) == -signal.SIGKILL, "expected_owned_crash_missing")
            crashed.close()

            # Do not unlink the mapping or singleton sidecar between attempts.
            require(mapping_identity(path) == crash_identity, "crash_mapping_missing")
            successor, session = launch()
            successor_identity = ready(successor, session)
            require(successor_identity[0:2] != crash_identity[0:2]
                    and successor_identity[2] != crash_identity[2], "crash_restart_reused_mapping")
            successor.send(STOP)
            final_status(successor, "control_stopped", 0, deadline())
            successor.close()
            report["checks_completed"] += 1
            report["fresh_provider_instances"] = len(clocks)
            report["passed"] = True
        finally:
            for child in owned:
                child.close()
    report["elapsed_seconds"] = round(time.monotonic() - started, 3)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-loopback", action="store_true",
                        help="Explicitly authorize loopback UDP listeners and temporary empty IPC files")
    parser.add_argument("--executable", type=Path, required=True)
    parser.add_argument("--timeout-seconds", type=float, default=60)
    args = parser.parse_args()
    try:
        require(args.run_loopback, "explicit_loopback_opt_in_required")
        require(sys.platform.startswith("linux"), "linux_only")
        require(10 <= args.timeout_seconds <= 90, "timeout_out_of_bounds")
        executable = args.executable.resolve(strict=True)
        require(executable.is_file() and os.access(executable, os.X_OK), "executable_unavailable")
        result = check(executable, args.timeout_seconds)
        print(json.dumps(result, sort_keys=True))
        return 0
    except CheckFailure as failure:
        print(json.dumps({"schema": 1, "passed": False, "reason": str(failure),
                          "capture_verified": False, "obs_verified": False}, sort_keys=True))
        return 1
    except Exception:
        # Native logs, paths, tokens and exception text are intentionally private.
        print(json.dumps({"schema": 1, "passed": False, "reason": "local_check_error",
                          "capture_verified": False, "obs_verified": False}, sort_keys=True))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
