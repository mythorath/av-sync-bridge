#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Linux-only, isolated libOBS regression runner. Never opens production OBS.

Only processes created by this runner are signaled. Outputs stay in a new private
directory. Requires prebuilt synthetic tools/module, Xvfb, ffmpeg and ffprobe.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import signal
import statistics
import subprocess
import sys
import time

CASES = ("baseline", "lead-zero", "restart", "stall", "stagger", "hide", "mute", "rapid-mute")


def wait_marker(path: Path, marker: str, process: subprocess.Popen, timeout: float = 20) -> None:
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        if marker in path.read_text(errors="replace"):
            return
        if process.poll() is not None:
            raise RuntimeError(f"Harness exited before {marker}; inspect {path.name}")
        time.sleep(0.05)
    raise TimeoutError(f"Harness did not reach {marker}")


def stop_child(process: subprocess.Popen | None) -> None:
    if process is None or process.poll() is not None:
        return
    # SIGCONT also ensures a deliberately stalled producer can terminate.
    # Every child below starts a new session. Include only that child's helpers
    # (not an unrelated OBS or X server) when stopping an interrupted harness.
    try:
        if os.getpgid(process.pid) != process.pid:
            raise RuntimeError("Refusing to signal an unexpected child process group")
        os.killpg(process.pid, signal.SIGCONT)
        os.killpg(process.pid, signal.SIGTERM)
        try:
            process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=5)
    except ProcessLookupError:
        # Exit between poll/group lookup/signal is normal, not a cleanup failure.
        process.wait(timeout=5)


def check_mute_case(report: dict) -> dict:
    """Only known fixture event 3 is suppressed; never infer a convenient subset."""
    video = report.get("video_onsets_seconds", [])
    tracks = report.get("tracks", [])
    if len(video) != 6 or len(tracks) != 2 or not report.get("video_fingerprint", {}).get("valid"):
        return {"passed": False, "reason": "Invalid six-event video fixture"}
    desktop = tracks[0].get("onsets_seconds", [])
    mic = tracks[1].get("onsets_seconds", [])
    if not tracks[0].get("fingerprint", {}).get("valid"):
        return {"passed": False, "reason": "Invalid desktop event fingerprint"}
    if len(desktop) != 6 or len(mic) != 5:
        return {"passed": False, "reason": "Expected six desktop and five microphone markers"}
    mic_video = [value for index, value in enumerate(video) if index != 2]
    offsets = [[(a-v)*1000 for a, v in zip(desktop, video)],
               [(a-v)*1000 for a, v in zip(mic, mic_video)]]
    return {"passed": all(abs(value) <= 33.333 for track in offsets for value in track)
            and all(abs(statistics.median(track)) <= 16.667 for track in offsets),
            "expected_suppressed_mic_event_one_based": 3, "remaining_offsets_ms": offsets,
            "scope": "Synthetic immediate mute only; no real filters or privacy guarantee"}


def run_case(args: argparse.Namespace, case: str, directory: Path) -> dict:
    directory.mkdir(mode=0o700)
    ipc, recording = directory / "media.ipc", directory / "encoded.mkv"
    log_path = directory / "obs.log"
    # Leave room for AAC/container tail below the analyzer's strict120s ceiling.
    duration = 20 * args.cycles - 1
    command = ["xvfb-run", "-a", "-s", "-screen 0 800x600x24",
               str(args.build_dir / "avsync-obs-smoke"), str(ipc), str(recording),
               str(args.build_dir / "plugins/obs/avsync-obs.so"), str(args.obs_plugins),
               str(args.obs_data), str(duration), "--audio-lead-ms", "0" if case == "lead-zero" else "40",
               "--warmup-seconds", "10" if case == "restart" else "1",
               "--stagger-ms", "500" if case == "stagger" else "0",
               "--scenario", case if case in ("hide", "mute", "rapid-mute") else "baseline"]
    processes: list[subprocess.Popen] = []
    handles = []

    def producer(name: str, seconds: int) -> subprocess.Popen:
        handle = (directory / name).open("x")
        handles.append(handle)
        child = subprocess.Popen([str(args.build_dir / "avsync-synthetic"), "--path", str(ipc),
                                  "--duration", str(seconds), "--delay-ms", "2000"],
                                 stdout=handle, stderr=subprocess.STDOUT, start_new_session=True)
        processes.append(child)
        return child

    try:
        log = log_path.open("x")
        handles.append(log)
        harness = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
        processes.append(harness)
        sender = None
        if case == "stagger":
            wait_marker(log_path, "AVSYNC_VIDEO_CREATED", harness)
            sender = producer("producer.log", duration - 3)
        elif case == "restart":
            wait_marker(log_path, "AVSYNC_READY", harness)
            previous = producer("previous-producer.log", 30)
            time.sleep(4)
            stop_child(previous)
        wait_marker(log_path, "AVSYNC_RECORDING_STARTED", harness, 25)
        if sender is None:
            sender = producer("producer.log", duration - 3)
        if case == "stall":
            time.sleep(6)
            sender.send_signal(signal.SIGSTOP)
            time.sleep(0.4)
            sender.send_signal(signal.SIGCONT)
        harness_code = harness.wait(timeout=duration + 15)
        sender_code = sender.wait(timeout=10)
        if harness_code or sender_code:
            raise RuntimeError(f"Child failure: OBS={harness_code}, producer={sender_code}")
        analyzer = Path(__file__).with_name("measure_synthetic.py")
        result = subprocess.run([sys.executable, str(analyzer), str(recording), "--cycles", str(args.cycles),
                                 "--max-median-ms", "16.667", "--max-offset-ms", "33.333"],
                                capture_output=True, text=True, timeout=180)
        (directory / "measurement.json").write_text(result.stdout, encoding="utf-8")
        (directory / "measurement.stderr").write_text(result.stderr, encoding="utf-8")
        report = json.loads(result.stdout) if result.stdout else {}
        summary = {"case": case, "harness_exit": harness_code, "producer_exit": sender_code,
                   "analyzer_exit": result.returncode, "valid_marker_match": report.get("valid_marker_match"),
                   "timing_gate": report.get("timing_gate"), "passed": result.returncode == 0}
        if case in ("mute", "rapid-mute"):
            summary["mute_fixture"] = check_mute_case(report)
            summary["passed"] = result.returncode == 2 and summary["mute_fixture"]["passed"]
        # Absolute producer-clock check catches a common audio/video shift which
        # relative encoded offsets alone could miss. Muted mic has its own known
        # missing-event assertion above; do not force a six-event raw match.
        raw_analyzer = Path(__file__).with_name("measure_mixer_trace.py")
        raw_checks = []
        for track in (range(1) if case in ("mute", "rapid-mute") else range(2)):
            raw_result = subprocess.run([sys.executable, str(raw_analyzer),
                "--tracefile", str(recording) + f".mix{track}.csv",
                "--producer-log", str(directory / "producer.log"), "--cycles", str(args.cycles),
                "--max-offset-ms", "2", "--current-generation-window"],
                capture_output=True, text=True, timeout=30)
            (directory / f"raw{track}.json").write_text(raw_result.stdout, encoding="utf-8")
            (directory / f"raw{track}.stderr").write_text(raw_result.stderr, encoding="utf-8")
            raw_checks.append({"track": track, "exit": raw_result.returncode,
                               "passed": raw_result.returncode == 0})
        summary["raw_timing_checks"] = raw_checks
        summary["passed"] = summary["passed"] and all(item["passed"] for item in raw_checks)
        return summary
    finally:
        cleanup_errors = []
        for child in reversed(processes):
            try:
                stop_child(child)
            except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
                print(f"Child cleanup failed: {error}", file=sys.stderr, flush=True)
                cleanup_errors.append(str(error))
        for handle in handles:
            handle.close()
        if cleanup_errors:
            raise RuntimeError("Incomplete child cleanup: " + "; ".join(cleanup_errors))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--obs-plugins", type=Path, required=True)
    parser.add_argument("--obs-data", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True, help="New private directory; must not exist")
    parser.add_argument("--case", action="append", choices=CASES, dest="cases")
    parser.add_argument("--cycles", type=int, choices=range(1, 7), default=1)
    args = parser.parse_args()
    if sys.platform != "linux":
        parser.error("The isolated OBS runner supports Linux only")
    args.cases = args.cases or ["baseline"] * 3
    if args.cycles != 1 and any(case in ("mute", "rapid-mute") for case in args.cases):
        parser.error("Mute fixtures currently require one cycle")
    for name in ("build_dir", "obs_plugins", "obs_data", "output_dir"):
        setattr(args, name, getattr(args, name).absolute())
    args.output_dir.mkdir(mode=0o700)
    results = []
    for index, case in enumerate(args.cases, 1):
        try:
            result = run_case(args, case, args.output_dir / f"{index:02d}-{case}")
        except Exception as error:
            result = {"case": case, "passed": False, "error": str(error)}
        results.append(result)
        print(json.dumps(result), flush=True)
        (args.output_dir / "summary.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0 if all(result["passed"] for result in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
