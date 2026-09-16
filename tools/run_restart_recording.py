#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Record a generated predecessor/replacement in one isolated Linux libOBS run.

No devices, network, normal OBS profile or real microphone are opened. A4 is
fully queued before the predecessor stops and must never appear in the output.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import select
import signal
import subprocess
import sys
import time

from run_synthetic_suite import stop_child

MAX_LOG_BYTES = 4 * 1024 * 1024


def parse_display_number(data: bytes) -> str:
    # Xvfb -displayfd writes exactly one ASCII decimal display number and LF.
    # It chooses an unused display itself; never probe or reset a user's server.
    if (not data.endswith(b"\n") or not 2 <= len(data) <= 7 or
            not data[:-1].isdigit() or int(data[:-1]) > 65535):
        raise ValueError("invalid_private_display_number")
    return ":" + str(int(data[:-1]))


def wait_display_fd(fd: int, child: subprocess.Popen, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    data = bytearray()
    while time.monotonic() < deadline:
        if child.poll() is not None:
            raise RuntimeError("private_display_exited_during_startup")
        readable, _, _ = select.select([fd], [], [], min(.1, max(0, deadline - time.monotonic())))
        if not readable:
            continue
        chunk = os.read(fd, 32)
        if not chunk:
            raise RuntimeError("private_display_startup_eof")
        data.extend(chunk)
        if len(data) > 32:
            raise ValueError("private_display_startup_limit")
        if b"\n" in data:
            display = parse_display_number(bytes(data))
            if child.poll() is not None:
                raise RuntimeError("private_display_exited_during_startup")
            return display
    raise TimeoutError("private_display_startup_timeout")


def read_log(path: Path) -> str:
    with path.open("rb") as handle:
        data = handle.read(MAX_LOG_BYTES + 1)
    if len(data) > MAX_LOG_BYTES:
        raise ValueError("fixture_log_limit")
    return data.decode("utf-8", errors="strict")


def record_fields(log: str, prefix: str) -> dict[str, int]:
    lines = [line for line in log.splitlines() if line.startswith(prefix + " ")]
    if len(lines) != 1:
        raise ValueError("missing_or_duplicate_fixture_record")
    result = {}
    # The producer emits path last; it may legitimately contain spaces. It is
    # diagnostic-only and is never part of the public timing proof.
    metadata = lines[0].partition(" path=")[0]
    seen = set()
    for token in metadata.split()[1:]:
        key, separator, value = token.partition("=")
        if not separator or key in seen:
            raise ValueError("invalid_fixture_field")
        seen.add(key)
        # The private path is deliberately not returned or copied to reports.
        if key == "size":
            if value != "640x360":
                raise ValueError("invalid_fixture_size")
            continue
        if not value.isascii() or not value.isdecimal() or len(value) > 20:
            raise ValueError("invalid_fixture_integer")
        parsed = int(value)
        if parsed > (2**64 - 1 if key == "generation" else 2**63 - 1):
            raise ValueError("fixture_integer_overflow")
        result[key] = parsed
    return result


def validate_predecessor(header: dict, barrier: dict) -> None:
    if (header.get("fixture_id") != 1 or header.get("fps") != 60 or
            header.get("delay_ms") != 2000 or not header.get("generation") or
            header.get("epoch_ns", 0) <= 0 or
            header.get("ready_ns", 0) < header["epoch_ns"]):
        raise ValueError("invalid_predecessor_header")
    if (barrier.get("fixture_id") != 1 or barrier.get("marker") != 4 or
            barrier.get("generation") != header["generation"] or
            barrier.get("presentation_start_ns") != header["epoch_ns"] + 8_700_000_000 or
            barrier.get("video_frames", 0) < 408 or barrier.get("audio_blocks", 0) < 674 or
            not header["epoch_ns"] + 6_700_000_000 <= barrier.get("barrier_ns", 0) <=
                barrier["presentation_start_ns"] - 1_000_000_000):
        raise ValueError("invalid_or_late_queued_barrier")


def validate_successor(previous: dict, barrier: dict, successor: dict,
                       stop_ns: int, reaped_ns: int) -> dict:
    validate_predecessor(previous, barrier)
    if (successor.get("fixture_id") != 2 or successor.get("fps") != 60 or
            successor.get("delay_ms") != 2000 or not successor.get("generation") or
            successor["generation"] == previous["generation"] or
            not barrier["barrier_ns"] <= stop_ns <= reaped_ns <=
                successor.get("epoch_ns", 0) <= successor.get("ready_ns", 0) or
            successor["ready_ns"] + 500_000_000 >= barrier["presentation_start_ns"]):
        raise ValueError("invalid_or_late_successor")
    return {"queued_predecessor_event": 4, "exact_predecessor_reaped": True,
            "fresh_generation": True,
            "replacement_margin_ms": (barrier["presentation_start_ns"] - successor["ready_ns"]) / 1e6,
            "replacement_after_stop_ms": (successor["ready_ns"] - stop_ns) / 1e6}


def wait_marker(path: Path, marker: str, child: subprocess.Popen, timeout: float) -> str:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        content = read_log(path)
        if any(line.startswith(marker + " ") for line in content.splitlines()):
            return content
        if child.poll() is not None:
            raise RuntimeError("fixture_child_exited_before_marker")
        time.sleep(0.01)
    raise TimeoutError("fixture_marker_timeout")


def run_case(args: argparse.Namespace, mode: str, directory: Path) -> dict:
    directory.mkdir(mode=0o700)
    ipc, recording = directory / "media.ipc", directory / "encoded.mkv"
    children, handles = [], []

    def launch(argv: list[str], name: str, *, env=None, pass_fds=()) -> subprocess.Popen:
        handle = (directory / name).open("x")
        handles.append(handle)
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=handle,
                                 stderr=subprocess.STDOUT, start_new_session=True,
                                 env=env, pass_fds=pass_fds)
        children.append(child)
        return child

    try:
        # Own Xvfb and libOBS separately. A wrapper exiting early must not hide
        # a surviving display helper from this runner's finally cleanup.
        read_fd, write_fd = os.pipe()
        try:
            xvfb = launch(["Xvfb", "-displayfd", str(write_fd), "-screen", "0",
                           "800x600x24", "-nolisten", "tcp", "-noreset"],
                          "xvfb.log", pass_fds=(write_fd,))
            os.close(write_fd)
            write_fd = None
            display = wait_display_fd(read_fd, xvfb, 10)
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)
        private_env = os.environ.copy()
        private_env["DISPLAY"] = display
        # The generated-only server has no connection to a desktop login's
        # authority file. Do not pass credentials for the user's normal display.
        private_env.pop("XAUTHORITY", None)
        harness = launch([str(args.build_dir / "avsync-obs-smoke"), str(ipc), str(recording),
            str(args.build_dir / "plugins/obs/avsync-obs.so"), str(args.obs_plugins),
            str(args.obs_data), "28", "--audio-lead-ms", "40", "--warmup-seconds", "1",
            "--scenario", "baseline"], "obs.log", env=private_env)
        wait_marker(directory / "obs.log", "AVSYNC_RECORDING_STARTED", harness, 25)
        common = [str(args.build_dir / "avsync-synthetic"), "--path", str(ipc), "--delay-ms", "2000"]
        predecessor = launch(common + ["--duration", "60", "--restart-fixture", "1"], "predecessor.log")
        predecessor_log = wait_marker(directory / "predecessor.log", "RESTART_QUEUED", predecessor, 10)
        previous = record_fields(predecessor_log, "SYNTHETIC")
        barrier = record_fields(predecessor_log, "RESTART_QUEUED")
        validate_predecessor(previous, barrier)
        if (xvfb.poll() is not None or harness.poll() is not None or predecessor.poll() is not None):
            raise RuntimeError("fixture_child_exited_before_interruption")
        stop_ns = time.monotonic_ns()
        predecessor.send_signal(signal.SIGTERM if mode == "graceful" else signal.SIGKILL)
        predecessor_exit = predecessor.wait(timeout=1)
        reaped_ns = time.monotonic_ns()
        if predecessor_exit != (0 if mode == "graceful" else -signal.SIGKILL):
            raise RuntimeError("unexpected_predecessor_exit")
        # Preserve both mapping and singleton file: explicit replacement must
        # validate and retire them, including the crashed predecessor's queue.
        successor = launch(common + ["--duration", "18", "--restart-fixture", "2"], "successor.log")
        successor_log = wait_marker(directory / "successor.log", "SYNTHETIC", successor, 2)
        replacement = record_fields(successor_log, "SYNTHETIC")
        fence = validate_successor(previous, barrier, replacement, stop_ns, reaped_ns)
        private_proof = {"predecessor": previous, "barrier": barrier, "successor": replacement,
                         "stop_ns": stop_ns, "reaped_ns": reaped_ns, "fence": fence}
        (directory / "transition.json").write_text(json.dumps(private_proof, indent=2) + "\n", encoding="utf-8")
        harness_exit = harness.wait(timeout=35)
        successor_exit = successor.wait(timeout=5)
        if harness_exit or successor_exit:
            raise RuntimeError("fixture_child_failure")
        if xvfb.poll() is not None:
            raise RuntimeError("private_display_exited_before_cleanup")
        analysis = subprocess.run([sys.executable, str(Path(__file__).with_name("measure_restart_recording.py")),
            "--recording", str(recording), "--predecessor-log", str(directory / "predecessor.log"),
            "--successor-log", str(directory / "successor.log"),
            "--mixer-trace", str(recording) + ".mix0.csv"], capture_output=True, text=True, timeout=120)
        (directory / "measurement.json").write_text(analysis.stdout, encoding="utf-8")
        (directory / "measurement.stderr").write_text(analysis.stderr, encoding="utf-8")
        report = json.loads(analysis.stdout)
        return {"case": mode, "passed": analysis.returncode == 0 and report.get("passed") is True,
                "scope": "generated desktop audio/video in isolated OBS, not hardware or real microphone",
                "fence": fence, "measurement": report, "predecessor_exit": predecessor_exit,
                "successor_exit": successor_exit, "harness_exit": harness_exit,
                "private_display_running_before_cleanup": True}
    finally:
        failures = []
        for child in reversed(children):
            try:
                stop_child(child)
            except (OSError, RuntimeError, subprocess.TimeoutExpired):
                failures.append("owned_child_cleanup_failed")
        for handle in handles:
            handle.close()
        if failures:
            raise RuntimeError("owned_child_cleanup_failed")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    for option in ("build-dir", "obs-plugins", "obs-data", "output-dir"):
        parser.add_argument("--" + option, type=Path, required=True)
    parser.add_argument("--case", choices=("graceful", "crash"), action="append", dest="cases")
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("Linux-only isolated generated recording")
    args.cases = args.cases or ["graceful", "crash"]
    if len(args.cases) > 6:
        parser.error("At most six bounded cases")
    for name in ("build_dir", "obs_plugins", "obs_data", "output_dir"):
        setattr(args, name, getattr(args, name).absolute())
    os.umask(0o077)
    args.output_dir.mkdir(mode=0o700)
    results = []
    for index, mode in enumerate(args.cases, 1):
        try:
            result = run_case(args, mode, args.output_dir / f"{index:02d}-{mode}")
        except Exception as error:
            # Exception strings can carry local paths; retain them privately.
            (args.output_dir / f"{index:02d}-failure.txt").write_text(str(error), encoding="utf-8")
            result = {"case": mode, "passed": False, "failure_type": type(error).__name__}
        results.append(result)
        print(json.dumps(result), flush=True)
        (args.output_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
        if not result["passed"]:
            break
    return 0 if len(results) == len(args.cases) and all(item["passed"] for item in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
