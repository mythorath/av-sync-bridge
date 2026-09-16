#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Finite Linux receiver containment; explicit sessions, no installed startup.

Diagnostics retain their 180-second cap. A --session-seconds start explicitly
permits up to twelve hours with the unchanged native stdin lease and independent
retirement fence. The immutable intent's unique native duration flag records
which mode was authorized; old diagnostic intents keep their exact schema.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import selectors
import stat
import subprocess
import sys
import time
from typing import Any

if sys.platform == "linux":
    import fcntl

MAX_STATE = 32768
MAX_QUERY = 8192
RETIRE_SECONDS = 8.0
UINT64_MAX = (1 << 64) - 1
HEX = re.compile(r"[0-9a-f]{32}")
BOOT = re.compile(r"[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}")
PROPERTIES = (
    "Id", "LoadState", "ActiveState", "SubState", "InvocationID", "ControlGroup",
    "MainPID", "ControlPID", "Job", "Transient", "Type", "Restart", "KillMode",
    "SendSIGKILL", "RuntimeMaxUSec", "TimeoutStopUSec",
)


class RemoteError(Exception):
    """Only fixed, non-sensitive reason codes may leave this helper."""


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise RemoteError("duplicate_json_key")
        value[key] = item
    return value


def decode_json(data: bytes):
    def invalid_constant(_):
        raise RemoteError("invalid_json")
    try:
        return json.loads(data.decode("utf-8"), object_pairs_hook=unique_object,
                          parse_constant=invalid_constant)
    except (ValueError, UnicodeError, RecursionError):
        raise RemoteError("invalid_json") from None


def identity(run_id: str, session: str) -> None:
    if not isinstance(run_id, str) or not HEX.fullmatch(run_id):
        raise RemoteError("invalid_run_id")
    if (not isinstance(session, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", session)
            or int(session) > UINT64_MAX):
        raise RemoteError("invalid_sender_session")


def unit_name(run_id: str) -> str:
    return "avsync-receiver-" + run_id + ".service"


def boot_id() -> str:
    with open("/proc/sys/kernel/random/boot_id", "rb") as handle:
        value = handle.read(65).decode("ascii").strip()
    if not BOOT.fullmatch(value):
        raise RemoteError("invalid_boot_id")
    return value


def validate_argv(value: Any, *, session_mode: bool = False) -> tuple[str, ...]:
    if (not isinstance(value, list) or not 1 <= len(value) <= 96
            or any(not isinstance(arg, str) or not arg or len(arg) > 2048
                   or any(ord(ch) < 32 for ch in arg) for arg in value)
            or not value[0].startswith("/")):
        raise RemoteError("invalid_receiver_argv")
    if type(session_mode) is not bool:
        raise RemoteError("invalid_session_mode")
    duration_flag = "--session-seconds" if session_mode else "--seconds"
    other_flag = "--seconds" if session_mode else "--session-seconds"
    if any(arg == other_flag or arg.startswith(other_flag + "=") or
           arg.startswith(duration_flag + "=") for arg in value):
        raise RemoteError("duration_mode_mismatch")
    if session_mode and any(arg.split("=", 1)[0] in
            ("--recover-desktop-ipc", "--clock-pause-after", "--clock-pause-seconds") for arg in value):
        raise RemoteError("session_recovery_disabled")
    for flag, placeholder in (("--expect-sender-session", "{sender_session}"), (duration_flag, "{seconds}")):
        if value.count(flag) != 1 or value.index(flag) + 1 >= len(value) or value[value.index(flag) + 1] != placeholder:
            raise RemoteError("invalid_receiver_argv")
        if value.count(placeholder) != 1:
            raise RemoteError("invalid_receiver_argv")
    if value.count("--control-stdin") != 1:
        raise RemoteError("invalid_receiver_argv")
    if any(("{" in arg or "}" in arg) and arg not in ("{sender_session}", "{seconds}") for arg in value):
        raise RemoteError("invalid_receiver_argv")
    return tuple(value)


def validate_intent(value: Any, run_id: str, session: str) -> dict:
    keys = {"schema", "run_id", "sender_session", "boot_id", "unit", "seconds", "receiver_argv"}
    if (not isinstance(value, dict) or set(value) != keys or type(value["schema"]) is not int
            or value["schema"] != 1 or value["run_id"] != run_id or value["sender_session"] != session
            or value["unit"] != unit_name(run_id) or not isinstance(value["boot_id"], str)
            or not BOOT.fullmatch(value["boot_id"]) or type(value["seconds"]) is not int
            or not isinstance(value["receiver_argv"], list)):
        raise RemoteError("invalid_intent")
    # Derive only from the exact, subsequently validated native flag. This keeps
    # pre-session schema-1 intents valid and makes mode auditable without a
    # second mutable or potentially contradictory mode field.
    session_mode = "--session-seconds" in value["receiver_argv"]
    validate_argv(value["receiver_argv"], session_mode=session_mode)
    if not 1 <= value["seconds"] <= (43200 if session_mode else 180):
        raise RemoteError("invalid_intent")
    return value


def validate_cancel(value: Any, run_id: str, session: str) -> None:
    if (not isinstance(value, dict) or set(value) != {"schema", "run_id", "sender_session"}
            or type(value["schema"]) is not int or value["schema"] != 1
            or value["run_id"] != run_id or value["sender_session"] != session):
        raise RemoteError("invalid_cancellation")


def valid_cgroup(path: Any, unit: str) -> bool:
    return (isinstance(path, str) and len(path) <= 1024 and path.startswith("/")
            and path.endswith("/" + unit) and all(part not in ("", ".", "..")
                and re.fullmatch(r"[A-Za-z0-9_.:@\\-]+", part) for part in path.split("/")[1:]))


def validate_receipt(value: Any, intent: dict) -> dict:
    keys = {"schema", "run_id", "sender_session", "boot_id", "unit", "invocation_id", "cgroup", "cgroup_dev", "cgroup_ino"}
    if (not isinstance(value, dict) or set(value) != keys or type(value["schema"]) is not int
            or value["schema"] != 1 or any(value[name] != intent[name]
                for name in ("run_id", "sender_session", "boot_id", "unit"))
            or not isinstance(value["invocation_id"], str) or not HEX.fullmatch(value["invocation_id"])
            or int(value["invocation_id"], 16) == 0 or not valid_cgroup(value["cgroup"], intent["unit"])
            or any(type(value[name]) is not int or not 0 < value[name] <= UINT64_MAX
                   for name in ("cgroup_dev", "cgroup_ino"))):
        raise RemoteError("invalid_receipt")
    return value


def _private(st, directory: bool) -> None:
    if (st.st_uid != os.geteuid() or stat.S_IMODE(st.st_mode) & 0o077
            or (not stat.S_ISDIR(st.st_mode) if directory else not stat.S_ISREG(st.st_mode) or st.st_nlink != 1)):
        raise RemoteError("unsafe_private_state")


class Attempt:
    """All authorization and cancellation use the same never-replaced lock."""

    def __init__(self, state_dir: Path, run_id: str, session: str):
        identity(run_id, session)
        self.path, self.run_id, self.session = state_dir, run_id, session
        self.fd = None

    @contextmanager
    def locked(self, deadline: float):
        if not self.path.is_absolute():
            raise RemoteError("unsafe_private_state")
        flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
        root_fd = os.open(self.path, flags)
        attempt_fd = lock_fd = None
        try:
            _private(os.fstat(root_fd), True)
            try:
                os.mkdir(self.run_id, mode=0o700, dir_fd=root_fd)
                os.fsync(root_fd)
            except FileExistsError:
                pass
            attempt_fd = os.open(self.run_id, flags, dir_fd=root_fd)
            _private(os.fstat(attempt_fd), True)
            lock_fd = os.open("lock", os.O_RDWR | os.O_CREAT | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK,
                              0o600, dir_fd=attempt_fd)
            _private(os.fstat(lock_fd), False)
            while True:
                if time.monotonic() >= deadline:
                    raise RemoteError("lock_timeout")
                try:
                    fcntl.flock(lock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    time.sleep(min(.02, max(0, deadline - time.monotonic())))
            linked = os.stat("lock", dir_fd=attempt_fd, follow_symlinks=False)
            opened = os.fstat(lock_fd)
            if (linked.st_dev, linked.st_ino) != (opened.st_dev, opened.st_ino):
                raise RemoteError("lock_identity_changed")
            for linked, opened in ((os.stat(self.path, follow_symlinks=False), os.fstat(root_fd)),
                                   (os.stat(self.run_id, dir_fd=root_fd, follow_symlinks=False), os.fstat(attempt_fd))):
                if (linked.st_dev, linked.st_ino) != (opened.st_dev, opened.st_ino):
                    raise RemoteError("state_directory_identity_changed")
            self.fd = attempt_fd
            yield self
        finally:
            self.fd = None
            for fd in (lock_fd, attempt_fd, root_fd):
                if fd is not None:
                    os.close(fd)

    def read(self, name: str):
        try:
            fd = os.open(name, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK, dir_fd=self.fd)
        except FileNotFoundError:
            return None
        try:
            st = os.fstat(fd)
            _private(st, False)
            if st.st_size > MAX_STATE:
                raise RemoteError("state_too_large")
            data = bytearray()
            while chunk := os.read(fd, min(4096, MAX_STATE + 1 - len(data))):
                data.extend(chunk)
                if len(data) > MAX_STATE:
                    raise RemoteError("state_too_large")
            return decode_json(bytes(data))
        finally:
            os.close(fd)

    def create(self, name: str, value: dict) -> None:
        data = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=True).encode("ascii")
        if len(data) > MAX_STATE:
            raise RemoteError("state_too_large")
        # An incomplete file is deliberately left as a fail-closed tombstone.
        # No gate may execute until the completed receipt and directory are synced.
        fd = os.open(name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                     0o600, dir_fd=self.fd)
        try:
            view = memoryview(data)
            while view:
                written = os.write(fd, view)
                if written <= 0:
                    raise RemoteError("state_write_failed")
                view = view[written:]
            os.fsync(fd)
            os.fsync(self.fd)
        finally:
            os.close(fd)


def load_config(path: Path, *, session_mode: bool = False) -> tuple[str, ...]:
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
    try:
        st = os.fstat(fd)
        _private(st, False)
        if st.st_size > MAX_STATE:
            raise RemoteError("config_too_large")
        with os.fdopen(fd, "rb", closefd=False) as handle:
            raw = decode_json(handle.read(MAX_STATE + 1))
        if not isinstance(raw, dict) or set(raw) != {"receiver_argv"}:
            raise RemoteError("invalid_configuration")
        return validate_argv(raw["receiver_argv"], session_mode=session_mode)
    finally:
        os.close(fd)


def prepare_intent(store, argv: tuple[str, ...], seconds: int, boot: str, deadline: float,
                   *, session_mode: bool = False) -> dict:
    if (type(session_mode) is not bool or type(seconds) is not int
            or not 1 <= seconds <= (43200 if session_mode else 180) or not BOOT.fullmatch(boot)):
        raise RemoteError("invalid_start")
    validate_argv(list(argv), session_mode=session_mode)
    with store.locked(deadline):
        cancel = store.read("cancel.json")
        if cancel is not None:
            validate_cancel(cancel, store.run_id, store.session)
            raise RemoteError("attempt_cancelled")
        if store.read("intent.json") is not None or store.read("receipt.json") is not None:
            raise RemoteError("attempt_already_reserved")
        intent = {"schema": 1, "run_id": store.run_id, "sender_session": store.session,
                  "boot_id": boot, "unit": unit_name(store.run_id), "seconds": seconds, "receiver_argv": list(argv)}
        store.create("intent.json", intent)
        return intent


def bounded_command(argv: list[str], deadline: float) -> tuple[int, bytes]:
    if time.monotonic() >= deadline:
        raise RemoteError("query_timeout")
    process = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.DEVNULL, close_fds=True, bufsize=0,
                               env={**os.environ, "LC_ALL": "C"})
    data = bytearray()
    try:
        with selectors.DefaultSelector() as selector:
            os.set_blocking(process.stdout.fileno(), False)
            selector.register(process.stdout, selectors.EVENT_READ)
            while selector.get_map():
                if time.monotonic() >= deadline:
                    raise RemoteError("query_timeout")
                for key, _ in selector.select(min(.05, max(0, deadline - time.monotonic()))):
                    chunk = os.read(key.fd, 4096)
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        data.extend(chunk)
                        if len(data) > MAX_QUERY:
                            raise RemoteError("query_output_limit")
        try:
            code = process.wait(timeout=max(.001, deadline - time.monotonic()))
        except subprocess.TimeoutExpired:
            raise RemoteError("query_timeout") from None
        return code, bytes(data)
    finally:
        if process.poll() is None:
            process.kill()  # Exact locally spawned query child, never the receiver PID.
            try:
                process.wait(timeout=.2)
            except subprocess.TimeoutExpired:
                pass
        process.stdout.close()


def parse_show(code: int, data: bytes, unit: str) -> dict[str, str]:
    try:
        lines = data.decode("ascii").splitlines()
    except UnicodeError:
        raise RemoteError("invalid_unit_query") from None
    values = {}
    for line in lines:
        key, separator, value = line.partition("=")
        if not separator or key not in PROPERTIES or key in values:
            raise RemoteError("invalid_unit_query")
        values[key] = value
    if values.get("Id") != unit:
        raise RemoteError("unit_identity_mismatch")
    if values.get("LoadState") == "not-found" and code in (0, 1, 4):
        return values
    if code != 0 or set(values) != set(PROPERTIES) or values["LoadState"] != "loaded":
        raise RemoteError("unit_query_failed")
    return values


def duration_us(value: str) -> int:
    parts = value.split()
    scales = {"us": 1, "ms": 1000, "s": 1_000_000, "min": 60_000_000, "h": 3_600_000_000}
    total = 0
    if not parts or len(parts) > 5:
        raise RemoteError("invalid_unit_duration")
    for part in parts:
        match = re.fullmatch(r"([0-9]{1,12})(us|ms|s|min|h)", part)
        if not match:
            raise RemoteError("invalid_unit_duration")
        total += int(match[1]) * scales[match[2]]
    return total


class Systemd:
    def show(self, unit: str, deadline: float) -> dict[str, str]:
        code, data = bounded_command(["systemctl", "--user", "show", "--no-pager",
                                     "--property=" + ",".join(PROPERTIES), "--", unit],
                                    min(deadline, time.monotonic() + 2))
        return parse_show(code, data, unit)

    def stop(self, unit: str, deadline: float) -> None:
        code, _ = bounded_command(["systemctl", "--user", "--no-block", "stop", "--", unit],
                                  min(deadline, time.monotonic() + 2))
        if code:
            raise RemoteError("unit_stop_failed")

    def cgroup(self, receipt: dict) -> str:
        path = Path("/sys/fs/cgroup") / receipt["cgroup"].lstrip("/")
        try:
            fd = os.open(path, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        except FileNotFoundError:
            return "absent"
        try:
            st = os.fstat(fd)
            if (st.st_dev, st.st_ino) != (receipt["cgroup_dev"], receipt["cgroup_ino"]):
                raise RemoteError("cgroup_identity_changed")
            events_fd = os.open("cgroup.events", os.O_RDONLY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            try:
                raw = os.read(events_fd, 1025)
            finally:
                os.close(events_fd)
            if len(raw) > 1024:
                raise RemoteError("invalid_cgroup_events")
            lines = raw.decode("ascii").splitlines()
            populated = [line for line in lines if line.startswith("populated ")]
            if populated == ["populated 0"]:
                return "empty"
            if populated == ["populated 1"]:
                return "populated"
            raise RemoteError("invalid_cgroup_events")
        finally:
            os.close(fd)

    def gate_identity(self, intent: dict, deadline: float) -> dict:
        invocation = os.environ.get("INVOCATION_ID", "")
        if not HEX.fullmatch(invocation) or int(invocation, 16) == 0:
            raise RemoteError("gate_not_in_expected_unit")
        with open("/proc/self/cgroup", "rb") as handle:
            raw = handle.read(4097)
        if len(raw) > 4096:
            raise RemoteError("invalid_gate_cgroup")
        groups = [line[3:] for line in raw.decode("ascii").splitlines() if line.startswith("0::")]
        if len(groups) != 1 or not valid_cgroup(groups[0], intent["unit"]):
            raise RemoteError("invalid_gate_cgroup")
        show = self.show(intent["unit"], deadline)
        expected = {"InvocationID": invocation, "ControlGroup": groups[0], "MainPID": str(os.getpid()),
                    "Transient": "yes", "Type": "exec", "Restart": "no", "KillMode": "control-group",
                    "SendSIGKILL": "yes"}
        if any(show.get(key) != value for key, value in expected.items()):
            raise RemoteError("gate_unit_policy_mismatch")
        if (duration_us(show["TimeoutStopUSec"]) != 2_000_000
                or duration_us(show["RuntimeMaxUSec"]) != (intent["seconds"] + 5) * 1_000_000):
            raise RemoteError("gate_unit_policy_mismatch")
        st = os.stat(Path("/sys/fs/cgroup") / groups[0].lstrip("/"), follow_symlinks=False)
        if not stat.S_ISDIR(st.st_mode):
            raise RemoteError("invalid_gate_cgroup")
        return {"invocation_id": invocation, "cgroup": groups[0], "cgroup_dev": st.st_dev, "cgroup_ino": st.st_ino}


def authorize_gate(store, backend, current_boot: str, deadline: float, execute) -> None:
    with store.locked(deadline):
        cancel = store.read("cancel.json")
        if cancel is not None:
            validate_cancel(cancel, store.run_id, store.session)
            raise RemoteError("attempt_cancelled")
        intent = validate_intent(store.read("intent.json"), store.run_id, store.session)
        if intent["boot_id"] != current_boot or store.read("receipt.json") is not None:
            raise RemoteError("gate_identity_reused")
        receipt = {name: intent[name] for name in ("schema", "run_id", "sender_session", "boot_id", "unit")}
        receipt.update(backend.gate_identity(intent, deadline))
        validate_receipt(receipt, intent)
        store.create("receipt.json", receipt)
        substitutions = {"{sender_session}": store.session, "{seconds}": str(intent["seconds"])}
        argv = [substitutions.get(arg, arg) for arg in intent["receiver_argv"]]
        # The authorization lock remains held through exec and closes atomically
        # through CLOEXEC. Retire cannot inspect a missing receipt and race exec.
        execute(argv)
        raise RemoteError("gate_exec_returned")


def cancellation_snapshot(store, deadline: float) -> tuple[dict | None, dict | None]:
    with store.locked(deadline):
        intent = store.read("intent.json")
        if intent is not None:
            validate_intent(intent, store.run_id, store.session)
        receipt = store.read("receipt.json")
        if receipt is not None:
            if intent is None:
                raise RemoteError("receipt_without_intent")
            validate_receipt(receipt, intent)
        cancel = store.read("cancel.json")
        if cancel is None:
            store.create("cancel.json", {"schema": 1, "run_id": store.run_id, "sender_session": store.session})
        else:
            validate_cancel(cancel, store.run_id, store.session)
        return intent, receipt


def checked_unit(show: dict, receipt: dict) -> bool:
    if show.get("LoadState") == "not-found":
        return True
    if (show.get("InvocationID") != receipt["invocation_id"] or show.get("Id") != receipt["unit"]
            or show.get("ControlGroup") not in ("", receipt["cgroup"])
            or show.get("Restart") != "no" or show.get("Transient") != "yes"):
        raise RemoteError("unit_identity_mismatch")
    terminal = (show.get("ActiveState") in ("inactive", "failed") and show.get("MainPID") == "0"
                and show.get("ControlPID") == "0" and show.get("Job") in ("", "0", "0 /"))
    if not terminal and show.get("ControlGroup") != receipt["cgroup"]:
        raise RemoteError("unit_identity_mismatch")
    return terminal


def retire(store, backend, current_boot: str, challenge: str, deadline: float) -> dict:
    if not isinstance(challenge, str) or not HEX.fullmatch(challenge) or not BOOT.fullmatch(current_boot):
        raise RemoteError("invalid_retirement_request")
    intent, receipt = cancellation_snapshot(store, deadline)
    if intent is not None and intent["boot_id"] != current_boot:
        proof = "fenced_prior_boot"
    elif receipt is None:
        # More than absence: the same exclusive lock now protects a durable,
        # irreversible cancel fence, and every native exec first writes receipt.
        proof = "fenced_never_started"
    else:
        stopped = False
        while True:
            if time.monotonic() >= deadline:
                raise RemoteError("retirement_timeout")
            show = backend.show(receipt["unit"], deadline)
            terminal = checked_unit(show, receipt)
            empty = backend.cgroup(receipt) in ("empty", "absent")
            if terminal and empty:
                proof = "fenced_empty_cgroup"
                break
            if show.get("LoadState") == "not-found":
                raise RemoteError("unloaded_unit_still_populated")
            if not stopped:
                backend.stop(receipt["unit"], deadline)
                stopped = True
            time.sleep(min(.05, max(0, deadline - time.monotonic())))
    return {"schema": 1, "event": "receiver_retired", "run_id": store.run_id,
            "sender_session": store.session, "challenge": challenge, "proof": proof}


def start_argv(state_dir: Path, intent: dict) -> list[str]:
    return ["systemd-run", "--user", "--quiet", "--pipe", "--wait", "--expand-environment=no",
            "--unit=" + intent["unit"], "--property=Type=exec", "--property=Restart=no",
            "--property=RuntimeMaxSec=" + str(intent["seconds"] + 5), "--property=TimeoutStartSec=5",
            "--property=TimeoutStopSec=2", "--property=KillMode=control-group", "--property=SendSIGKILL=yes",
            "--", str(Path(sys.executable).resolve()), str(Path(__file__).resolve()), "gate",
            "--state-dir", str(state_dir), "--run-id", intent["run_id"],
            "--expect-sender-session", intent["sender_session"]]


def start(store, argv, seconds: int, *, session_mode: bool = False) -> int:
    intent = prepare_intent(store, argv, seconds, boot_id(), time.monotonic() + 5, session_mode=session_mode)
    args = start_argv(store.path, intent)
    # Replace the SSH-owned helper; do not leave an extra launcher child behind.
    # The native control pipes pass through systemd-run unchanged. Suppress its
    # potentially private stderr, restoring ours only if exec itself fails.
    original_error = os.dup(2)
    null_error = os.open(os.devnull, os.O_WRONLY | os.O_CLOEXEC)
    try:
        os.dup2(null_error, 2)
        os.execvpe(args[0], args, {**os.environ, "LC_ALL": "C"})
        raise RemoteError("launcher_exec_returned")
    except OSError:
        os.dup2(original_error, 2)
        raise
    finally:
        os.close(original_error)
        os.close(null_error)


class Parser(argparse.ArgumentParser):
    def error(self, message):
        raise RemoteError("invalid_arguments")


def main() -> int:
    try:
        parser = Parser(description=__doc__)
        commands = parser.add_subparsers(dest="command", required=True, parser_class=Parser)
        for name in ("start", "retire", "gate"):
            command = commands.add_parser(name)
            command.add_argument("--state-dir", type=Path, required=True)
            command.add_argument("--run-id", required=True)
            command.add_argument("--expect-sender-session", required=True)
            if name == "start":
                command.add_argument("--config", type=Path, required=True)
                duration = command.add_mutually_exclusive_group(required=True)
                duration.add_argument("--seconds", type=int)
                duration.add_argument("--session-seconds", type=int)
                command.add_argument("--control-stdin", action="store_true", required=True)
            elif name == "retire":
                command.add_argument("--challenge", required=True)
        args = parser.parse_args()
        if sys.platform != "linux":
            raise RemoteError("linux_required")
        identity(args.run_id, args.expect_sender_session)
        store = Attempt(args.state_dir, args.run_id, args.expect_sender_session)
        if args.command == "start":
            session_mode = args.session_seconds is not None
            seconds = args.session_seconds if session_mode else args.seconds
            return start(store, load_config(args.config, session_mode=session_mode), seconds, session_mode=session_mode)
        if args.command == "gate":
            authorize_gate(store, Systemd(), boot_id(), time.monotonic() + 5,
                           lambda argv: os.execv(argv[0], argv))
            return 1
        proof = retire(store, Systemd(), boot_id(), args.challenge, time.monotonic() + RETIRE_SECONDS)
        line = "AVSYNC_RETIRE " + json.dumps(proof, ensure_ascii=True, separators=(",", ":"))
        if len(line) > 2048:
            raise RemoteError("proof_output_limit")
        print(line, flush=True)
        return 0
    except RemoteError as error:
        print(json.dumps({"schema": 1, "status": "remote_control_failed", "reason": str(error)}), file=sys.stderr)
        return 2
    except (OSError, ValueError, UnicodeError):
        print('{"schema":1,"status":"remote_control_failed","reason":"local_operation_failed"}', file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
