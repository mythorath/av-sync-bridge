#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run finite generated-signal checks, including deliberately wrong-rate controls.

No hardware, playback, network, OBS configuration, or PCM files are used.
Each child emits one aggregate JSON report. Reports stay on stdout.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess


def cases(quick: bool = False, long: bool = False) -> list[dict]:
    result = []

    def add(signal: str, ppm: int, frequency: int = 1000, varied: bool = False,
            command: int | None = None, estimate: bool = False, seconds: int = 8) -> None:
        result.append(dict(signal=signal, ppm=ppm, frequency=frequency, varied=varied,
                           command=command, estimate=estimate, seconds=seconds,
                           expected_pass=command is None or command == ppm))

    if quick:
        add("tone", 500, seconds=4)
        add("markers", -500, varied=True, seconds=4)
        add("tone", 100, 18000, estimate=True, seconds=4)
        add("silence", 500, command=0, seconds=4)
        add("dc", -500, command=0, seconds=4)
        return result
    for ppm in (0, -500, -100, 100, 500):
        for varied in (False, True):
            for frequency in (1000, 10000, 18000):
                add("tone", ppm, frequency, varied)
            for signal in ("markers", "silence", "dc"):
                add(signal, ppm, varied=varied)
    for ppm in (-499, -100, 0, 100, 499):
        add("markers", ppm, varied=True, estimate=True)
        add("tone", ppm, 1000, estimate=True)
    for signal in ("tone", "markers", "silence", "dc"):
        add(signal, 500, command=0)
        add(signal, -500, varied=True, command=0)
    if long:
        add("markers", 500, seconds=600)
    return result


def valid_result(case: dict, returncode: int, report: dict) -> bool:
    expected = case["expected_pass"]
    if not isinstance(report, dict):
        return False
    basic = (returncode == (0 if expected else 3)
            and report.get("schema") == 1
            and report.get("generated_only") is True
            and report.get("live_controller") is False
            and report.get("passed") is expected
            and report.get("source_ppm") == case["ppm"]
            and report.get("signal") == case["signal"]
            and report.get("seconds") == case["seconds"]
            and report.get("frequency_hz") == case["frequency"]
            and report.get("chunk_pattern") == ("varied" if case["varied"] else "fixed")
            and report.get("original_anchor_preflight") is case["estimate"])
    integer_fields = ("input_frames", "output_frames", "source_duration_error_frames", "count_error_frames")
    if not basic or any(type(report.get(key)) is not int for key in integer_fields):
        return False
    if report["input_frames"] <= 0 or report["output_frames"] <= 0:
        return False
    # Independent integer arithmetic, not the fixture's command-based expectation.
    exact_expected = report["input_frames"] * 1_000_000 // (1_000_000 + case["ppm"])
    error = report["output_frames"] - exact_expected
    if report["source_duration_error_frames"] != error:
        return False
    if not expected:
        return abs(error) > 1
    fields = ("residual_rms_dbfs", "maximum_residual_dbfs", "right_peak_dbfs", "gain_db",
              "max_marker_error_samples", "output_peak")
    if any(type(report.get(key)) not in (int, float) or not math.isfinite(report[key]) for key in fields):
        return False
    return (abs(error) <= 1 and abs(report["count_error_frames"]) <= 1
            and report["residual_rms_dbfs"] <= -80
            and report["maximum_residual_dbfs"] <= -50 + 1e-5
            and report["right_peak_dbfs"] <= -100
            and abs(report["gain_db"]) <= 0.2
            and 0 <= report["max_marker_error_samples"] <= 1
            and 0 <= report["output_peak"] <= 1
            and (case["signal"] != "markers" or report.get("marker_regions") == 6)
            and (case["signal"] != "silence" or report["output_peak"] <= 1e-7))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--quick", action="store_true")
    group.add_argument("--long", action="store_true", help="Add one600s simulated constant-rate marker case")
    args = parser.parse_args()
    results = []
    for number, case in enumerate(cases(args.quick, args.long), 1):
        command = [str(args.executable.absolute()), "--offline", "--seconds", str(case["seconds"]),
                   "--ppm", str(case["ppm"]), "--frequency", str(case["frequency"]),
                   "--signal", case["signal"], "--chunk-pattern", "varied" if case["varied"] else "fixed"]
        if case["command"] is not None:
            command += ["--command-ppm", str(case["command"])]
        if case["estimate"]:
            command.append("--estimate")
        try:
            child = subprocess.run(command, capture_output=True, text=True, timeout=130)
            report = json.loads(child.stdout)
            passed = valid_result(case, child.returncode, report)
            result = dict(case=number, parameters=case, passed=passed,
                          exit=child.returncode, report=report)
        except (OSError, subprocess.TimeoutExpired, ValueError, TypeError) as error:
            # Avoid publishing executable paths, exception text, or environmental logs.
            result = dict(case=number, parameters=case, passed=False, error=type(error).__name__)
        results.append(result)
        print(json.dumps(result, allow_nan=False), flush=True)
    passed = all(item["passed"] for item in results)
    print(json.dumps(dict(summary=True, generated_only=True, cases=len(results),
                          passed=passed, failures=sum(not item["passed"] for item in results))), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
