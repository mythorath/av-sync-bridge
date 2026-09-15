#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded, generated-only nominal PCM conversion tests; JSON reports to stdout.

This is not a live A/V test or an adaptive clock-correction test. The original
capture clock metadata is kept separate from the nominal output sample timeline.
"""
from __future__ import annotations

import argparse
import json
import math
from pathlib import Path
import subprocess


def cases(quick: bool = False, long: bool = False) -> list[dict]:
    result = []

    def add(rate: int, signal: str, ppm: int = 0, frequency: int = 1000,
            seconds: int = 2, negative: bool = False) -> None:
        result.append(dict(rate=rate, signal=signal, ppm=ppm, frequency=frequency,
                           seconds=seconds, negative=negative))

    if quick:
        add(192000, "tone", 500)
        add(44100, "markers", -500)
        add(192000, "impulse")
        add(48000, "silence", 500)
        add(44100, "tone", negative=True)
        return result
    for rate in (192000, 44100, 48000):
        for ppm in (-500, 0, 500):
            for frequency in (1000, 10000, 18000):
                add(rate, "tone", ppm, frequency)
            for signal in ("markers", "impulse", "silence"):
                add(rate, signal, ppm)
    add(192000, "tone", negative=True)
    add(44100, "markers", negative=True)
    if long:
        for ppm in (-500, 500):
            add(192000, "markers", ppm, seconds=90)
        add(44100, "tone", 500, seconds=90)
    return result


def valid_result(case: dict, returncode: int, report: dict) -> bool:
    if not isinstance(report, dict):
        return False
    expected = not case["negative"]
    basic = (returncode == (0 if expected else 3)
             and report.get("schema") == 1
             and report.get("generated_only") is True
             and report.get("timestamps_passed_to_converter") is False
             and report.get("capture_metadata_only") is True
             and report.get("live_sync_proven") is False
             and report.get("passed") is expected
             and report.get("signal") == case["signal"]
             and report.get("seconds") == case["seconds"]
             and report.get("source_rate") == case["rate"]
             and report.get("source_channels") == (8 if case["rate"] == 192000 else 2)
             and report.get("frequency_hz") == case["frequency"]
             and report.get("source_ppm") == case["ppm"]
             and report.get("negative_control") is case["negative"])
    integers = ("input_frames", "fixed_output_frames", "varied_output_frames",
                "fixed_before_finish_frames", "varied_before_finish_frames",
                "original_capture_duration_ns", "nominal_output_duration_ns",
                "capture_minus_nominal_ns", "max_latency_input_frames")
    if not basic or any(type(report.get(key)) is not int for key in integers):
        return False
    target = case["seconds"] * 48000
    original_ns = case["seconds"] * 10**15 // (1_000_000 + case["ppm"])
    nominal_ns = case["seconds"] * 10**9
    # Independent exact count and clock arithmetic, not the fixture's pass flag.
    if not (report["input_frames"] == case["seconds"] * case["rate"]
            and report["fixed_output_frames"] == target
            and report["varied_output_frames"] == target
            and 0 < report["fixed_before_finish_frames"] <= target
            and report["fixed_before_finish_frames"] == report["varied_before_finish_frames"]
            and report["original_capture_duration_ns"] == original_ns
            and report["nominal_output_duration_ns"] == nominal_ns
            and report["capture_minus_nominal_ns"] == original_ns - nominal_ns
            and 0 <= report["max_latency_input_frames"] <= case["rate"] // 10):
        return False
    fields = ("partition_max_error", "residual_rms_dbfs", "maximum_residual_dbfs",
              "fixed_gain_db", "varied_gain_db", "right_peak_dbfs", "output_peak",
              "max_marker_error_samples", "max_impulse_error_samples", "impulse_peak")
    if any(type(report.get(key)) not in (int, float) or not math.isfinite(report[key]) for key in fields):
        return False
    if not expected:
        return report["partition_max_error"] > 1e-7
    common = (0 <= report["partition_max_error"] <= 1e-7
              and report["right_peak_dbfs"] <= -140
              and 0 <= report["output_peak"] <= 1)
    if not common:
        return False
    if case["signal"] == "impulse":
        return (0 <= report["max_impulse_error_samples"] <= 1
                and report["impulse_peak"] > 0.05)
    analytic = (report["residual_rms_dbfs"] <= -80
                and report["maximum_residual_dbfs"] <= -50 + 1e-5
                and abs(report["fixed_gain_db"]) <= 0.2
                and abs(report["varied_gain_db"]) <= 0.2)
    if case["signal"] == "markers":
        analytic = analytic and report.get("marker_regions") == 6 and 0 <= report["max_marker_error_samples"] <= 1
    if case["signal"] == "silence":
        analytic = analytic and report["output_peak"] <= 1e-7
    return analytic


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", type=Path, required=True)
    group = parser.add_mutually_exclusive_group()
    group.add_argument("--quick", action="store_true")
    group.add_argument("--long", action="store_true", help="Include three accelerated 90s source-clock cases")
    args = parser.parse_args()
    results = []
    for number, case in enumerate(cases(args.quick, args.long), 1):
        command = [str(args.executable.absolute()), "--offline", "--seconds", str(case["seconds"]),
                   "--rate", str(case["rate"]), "--signal", case["signal"],
                   "--frequency", str(case["frequency"]), "--ppm", str(case["ppm"])]
        if case["negative"]:
            command.append("--shift-varied-one-sample")
        try:
            child = subprocess.run(command, capture_output=True, text=True, timeout=120)
            report = json.loads(child.stdout)
            passed = valid_result(case, child.returncode, report)
            result = dict(case=number, parameters=case, passed=passed,
                          exit=child.returncode, report=report)
        except (OSError, subprocess.TimeoutExpired, ValueError, TypeError) as error:
            # Do not publish local paths or unrelated environmental stderr.
            result = dict(case=number, parameters=case, passed=False, error=type(error).__name__)
        results.append(result)
        print(json.dumps(result, allow_nan=False), flush=True)
    passed = all(item["passed"] for item in results)
    print(json.dumps(dict(summary=True, generated_only=True, cases=len(results),
                          passed=passed, failures=sum(not item["passed"] for item in results))), flush=True)
    return 0 if passed else 1


if __name__ == "__main__":
    raise SystemExit(main())
