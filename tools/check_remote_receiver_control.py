#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit Linux/systemd containment check using generated, silent processes.

Never opens devices, network listeners, audio, OBS or installed services. Uses
new private state and temporary per-attempt user units. No process-name kills.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import select
import signal
import subprocess
import sys
import time

from retirement_proof import verify_retirement


def start_ticks(pid: int) -> int:
    text = Path(f"/proc/{pid}/stat").read_text()
    return int(text[text.rfind(")") + 2:].split()[19])


def fixture() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--fixture", action="store_true")
    parser.add_argument("--descendant", action="store_true")
    parser.add_argument("--stubborn", action="store_true")
    parser.add_argument("--state", type=Path)
    parser.add_argument("--expect-sender-session")
    parser.add_argument("--seconds", type=int, required=True)
    parser.add_argument("--control-stdin", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.seconds <= 180:
        return 2
    if args.stubborn or args.descendant:
        signal.signal(signal.SIGTERM, signal.SIG_IGN)
    if args.descendant:
        time.sleep(args.seconds)
        return 0
    children = []
    if args.stubborn:
        children.append(subprocess.Popen([sys.executable, str(Path(__file__).resolve()),
            "--fixture", "--descendant", "--seconds", str(args.seconds)],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL))
    identities = [{"pid": pid, "start_ticks": start_ticks(pid)}
                  for pid in [os.getpid()] + [child.pid for child in children]]
    with (args.state / "fixture-processes.json").open("x", encoding="utf-8") as output:
        json.dump(identities, output)
    print("AVSYNC_CONTROL " + json.dumps({"schema": 1, "event": "receiver_ready",
        "clock_epoch": str(secrets.randbits(64) or 1), "sender_session": args.expect_sender_session}), flush=True)
    deadline = time.monotonic() + args.seconds
    while time.monotonic() < deadline:
        if args.stubborn:
            time.sleep(.02)
            continue
        if select.select([sys.stdin], [], [], .1)[0]:
            data = os.read(sys.stdin.fileno(), 1024)
            if not data:
                return 1
            if b"AVSYNC_STOP\n" in data:
                return 0
    return 0


def wait_ready(child: subprocess.Popen, session: str, timeout: float = 8) -> None:
    deadline, data = time.monotonic() + timeout, bytearray()
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError("fixture_exited_before_ready")
        if not select.select([child.stdout], [], [], .05)[0]:
            continue
        chunk = os.read(child.stdout.fileno(), 1024)
        if not chunk:
            raise RuntimeError("fixture_output_closed")
        data.extend(chunk)
        if len(data) > 2048:
            raise RuntimeError("fixture_ready_output_limit")
        if b"\n" in data:
            if not data.startswith(b"AVSYNC_CONTROL "):
                raise RuntimeError("unexpected_fixture_ready")
            message = json.loads(data[15:])
            if (not isinstance(message, dict) or set(message) != {"schema", "event", "clock_epoch", "sender_session"}
                    or type(message["schema"]) is not int or message["schema"] != 1
                    or message["event"] != "receiver_ready" or message["sender_session"] != session
                    or not isinstance(message["clock_epoch"], str)
                    or not re.fullmatch(r"[1-9][0-9]{0,19}", message["clock_epoch"])
                    or int(message["clock_epoch"]) >= 2**64):
                raise RuntimeError("unexpected_fixture_ready")
            return
    raise TimeoutError("fixture_ready_timeout")


def open_fixture_pidfds(directory: Path, expected_count: int) -> list[int]:
    handles = []
    try:
        with (directory / "fixture-processes.json").open() as stream:
            identities = json.load(stream)
        if (expected_count not in (1, 2) or not isinstance(identities, list) or len(identities) != expected_count
                or len({identity["pid"] for identity in identities}) != expected_count):
            raise RuntimeError("invalid_fixture_identity_count")
        for identity in identities:
            pid, ticks = identity["pid"], identity["start_ticks"]
            if type(pid) is not int or type(ticks) is not int or pid <= 1 or ticks <= 0 or start_ticks(pid) != ticks:
                raise RuntimeError("invalid_fixture_identity")
            handle = os.pidfd_open(pid)
            handles.append(handle)
            if start_ticks(pid) != ticks or select.select([handle], [], [], 0)[0]:
                raise RuntimeError("fixture_identity_changed_or_exited")
        return handles
    except BaseException:
        for handle in handles:
            os.close(handle)
        raise


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-systemd", action="store_true", required=True)
    parser.add_argument("--output-dir", required=True, type=Path)
    parser.add_argument("--helper", type=Path, default=Path(__file__).with_name("remote_receiver.py"))
    args = parser.parse_args()
    if sys.platform != "linux" or not hasattr(os, "pidfd_open"):
        parser.error("Linux with pidfd support required")
    os.umask(0o077)
    directory = args.output_dir.absolute()
    directory.mkdir(mode=0o700)
    state = directory / "remote-state"
    state.mkdir(mode=0o700)
    helper = [sys.executable, str(args.helper.absolute())]
    results = []

    def retire(run_id, session, timeout=10):
        challenge = secrets.token_hex(16)
        return verify_retirement(helper + ["retire", "--state-dir", str(state), "--run-id", run_id,
            "--expect-sender-session", session, "--challenge", challenge], run_id, session, challenge, timeout)

    for mode in ("cancel_before_start", "clean_stop", "lost_launcher_stubborn_group"):
        case = directory / mode
        case.mkdir(mode=0o700)
        run_id, session = secrets.token_hex(16), str(secrets.randbits(64) or 1)
        config = case / "receiver.json"
        command = [sys.executable, str(Path(__file__).resolve()), "--fixture", "--state", str(case),
            "--expect-sender-session", "{sender_session}", "--seconds", "{seconds}", "--control-stdin"]
        if mode == "lost_launcher_stubborn_group":
            command.append("--stubborn")
        config.write_text(json.dumps({"receiver_argv": command}), encoding="utf-8")
        launch = helper + ["start", "--state-dir", str(state), "--config", str(config), "--run-id", run_id,
            "--expect-sender-session", session, "--seconds", "20", "--control-stdin"]
        child, pidfds, stderr = None, [], None
        result = {"case": mode, "passed": False, "scope": "generated processes, no media or devices"}
        try:
            if mode == "cancel_before_start":
                proof = retire(run_id, session)
                result["retirement"] = proof
                if not proof["verified"] or proof["proof"] != "fenced_never_started":
                    raise RuntimeError("early_fence_not_verified")
                refused = subprocess.run(launch, input=b"", capture_output=True, timeout=10)
                if refused.returncode == 0 or b"AVSYNC_CONTROL" in refused.stdout or (case / "fixture-processes.json").exists():
                    raise RuntimeError("canceled_late_launch_not_refused")
                result.update(passed=True, retirement=proof, late_launch_refused=True)
            else:
                stderr = (case / "launcher.stderr").open("xb")
                child = subprocess.Popen(launch, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                         stderr=stderr, bufsize=0)
                wait_ready(child, session)
                pidfds = open_fixture_pidfds(case, 1 if mode == "clean_stop" else 2)
                if mode == "clean_stop":
                    child.stdin.write(b"AVSYNC_STOP\n")
                    child.stdin.flush()
                    child.wait(timeout=5)
                    if child.returncode != 0:
                        raise RuntimeError("clean_launcher_stop_failed")
                else:
                    child.kill()  # Only the exact foreground launcher, not native PIDs.
                    child.wait(timeout=3)
                    if any(select.select([handle], [], [], 0)[0] for handle in pidfds):
                        raise RuntimeError("stubborn_fixture_did_not_survive_launcher_loss")
                proof = retire(run_id, session)
                result["retirement"] = proof
                if not proof["verified"] or proof["proof"] != "fenced_empty_cgroup":
                    raise RuntimeError("remote_retirement_not_verified")
                if any(not select.select([handle], [], [], 1)[0] for handle in pidfds):
                    raise RuntimeError("owned_fixture_survived_claimed_retirement")
                result.update(passed=True, retirement=proof, pidfd_verified_exits=len(pidfds),
                              native_pid_kills_used=False)
            repeated = retire(run_id, session)
            if not repeated["verified"]:
                raise RuntimeError("fresh_idempotent_retirement_failed")
            result["fresh_repeated_proof"] = True
        except Exception as error:
            result.update(passed=False, failure_type=type(error).__name__)
            (case / "failure.txt").write_text(str(error), encoding="utf-8")
        finally:
            try:
                if child is not None and child.poll() is None:
                    child.kill()
                    child.wait(timeout=3)
            except (OSError, subprocess.TimeoutExpired):
                result.update(passed=False, launcher_cleanup_failed=True)
            # Local launcher failure must never skip the independent remote
            # fence/retirement. Keep both outcomes if cleanup has two failures.
            try:
                cleanup = retire(run_id, session)
                if not cleanup["verified"]:
                    result.update(passed=False, cleanup_verified=False)
            except (OSError, RuntimeError, ValueError):
                result.update(passed=False, cleanup_verified=False)
            for handle in pidfds:
                os.close(handle)
            if child is not None:
                child.stdin.close()
                child.stdout.close()
            if stderr:
                stderr.close()
        results.append(result)
        print(json.dumps(result), flush=True)
        (directory / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        if not result["passed"]:
            break
    return 0 if len(results) == 3 and all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(fixture() if "--fixture" in sys.argv else main())
