#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Compare a bounded raw OBS mixer RMS trace with one synthetic producer epoch.

Only relative timing is emitted: no input paths, producer epoch, generation, or
other private log values are copied to the JSON report. This is not a mux-epoch
or hardware-latency measurement. Requires only Python's standard library.
"""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys
from typing import Iterable

from measure_synthetic import EVENT_SECONDS, MeasurementError, expected_schedule, region_onsets, threshold_regions

MAX_ROWS = 150_000
MAX_TRACE_NS = 130_000_000_000
MAX_SIGNED_NS = (1 << 63) - 1
SAMPLE_RATE = 48_000


def parse_producer_log(text: str) -> dict:
    lines = [line for line in text.splitlines() if line.startswith("SYNTHETIC ")]
    if len(lines) != 1:
        raise MeasurementError("Require exactly one producer timing header; restart logs need separate analysis")
    fields = dict(re.findall(r"\b([a-z_]+)=([^\s]+)", lines[0]))
    try:
        epoch, delay, fps = (int(fields[key]) for key in ("epoch_ns", "delay_ms", "fps"))
    except (ValueError, KeyError) as exc:
        raise MeasurementError("Producer header is missing valid epoch/delay/fps fields") from exc
    if not 0 <= epoch <= MAX_SIGNED_NS or not 0 <= delay <= 10_000 or not 1 <= fps <= 120:
        raise MeasurementError("Producer timing header exceeds supported limits")
    return {"epoch_ns": epoch, "delay_ms": delay, "fps": fps}


def intended_offsets(header: dict, cycles: int) -> list[float]:
    expected_schedule(cycles)  # Validate the same explicit bounded cycle count.
    fps = header["fps"]
    times = []
    # Mirror the producer's integer frame rounding and subsequent sample-index
    # quantization, rather than assuming all frame rates hit decimal times exactly.
    for cycle in range(cycles):
        for event in EVENT_SECONDS:
            event_ms = round(event * 1000)
            frame = (event_ms * fps + 500) // 1000
            sample = frame * SAMPLE_RATE // fps + cycle * 20 * SAMPLE_RATE
            ns = header["delay_ms"] * 1_000_000 + sample * 1_000_000_000 // SAMPLE_RATE
            if header["epoch_ns"] > MAX_SIGNED_NS - ns:
                raise MeasurementError("Intended presentation timestamp would overflow")
            times.append(ns / 1e9)
    return times


def analyze_rows(rows: Iterable[dict], header: dict, *, cycles: int = 1,
                 max_offset_ms: float | None = None, interval_tolerance_ms: float = 40.0,
                 current_generation_window: bool = False) -> dict:
    schedule = intended_offsets(header, cycles)
    if max_offset_ms is not None and (not math.isfinite(max_offset_ms) or max_offset_ms < 0):
        raise MeasurementError("Maximum offset must be finite and nonnegative")
    if not math.isfinite(interval_tolerance_ms) or interval_tolerance_ms <= 0:
        raise MeasurementError("Interval tolerance must be finite and positive")
    times, levels, durations = [], [], []
    first_ns = previous_ns = None
    for number, row in enumerate(rows):
        if number >= MAX_ROWS:
            raise MeasurementError("Trace exceeds the bounded row count")
        try:
            timestamp, frames, rms = int(row["timestamp_ns"]), int(row["frames"]), float(row["rms"])
        except (TypeError, ValueError, KeyError) as exc:
            raise MeasurementError("Invalid trace row fields") from exc
        if (not 0 <= timestamp <= MAX_SIGNED_NS or not 1 <= frames <= 48 or
                not math.isfinite(rms) or rms < 0):
            raise MeasurementError("Trace requires finite RMS, signed nanoseconds, and 1..48 samples per window")
        if first_ns is None:
            first_ns = timestamp
        if (previous_ns is not None and timestamp <= previous_ns) or timestamp - first_ns > MAX_TRACE_NS:
            raise MeasurementError("Trace timestamps are non-increasing or exceed the duration limit")
        previous_ns = timestamp
        # Subtract integers before float conversion to retain relative precision.
        times.append((timestamp - header["epoch_ns"]) / 1e9)
        levels.append(rms)
        durations.append(frames / SAMPLE_RATE)
    if not times or max(levels) < 0.005:
        raise MeasurementError("Trace is empty or contains no sufficiently strong synthetic tones")
    threshold = max(0.002, max(levels) * 0.12)
    regions = threshold_regions(times, levels, durations, threshold, bridge_gap=0.003)
    selection = {"mode": "all_trace", "ignored_prior_regions": 0, "ignored_later_regions": 0}
    if current_generation_window:
        start = header["delay_ms"] / 1000
        end = start + cycles * 20 - 4
        selection.update({"mode": "explicit_current_generation_window",
                          "start_relative_epoch_seconds": start, "end_relative_epoch_seconds": end,
                          "definition": "producer epoch + delay through that instant + (cycles * 20 - 4) seconds",
                          "boundary_policy": "reject straddling regions; never choose a passing subset"})
        selected = []
        for region in regions:
            if region["end_seconds"] <= start:
                selection["ignored_prior_regions"] += 1
            elif region["start_seconds"] >= end:
                selection["ignored_later_regions"] += 1
            elif region["start_seconds"] < start or region["end_seconds"] > end:
                raise MeasurementError("Detected region straddles the explicit generation window")
            else:
                selected.append(region)
        regions = selected
    onsets = region_onsets(regions, min_duration=0.010, max_duration=0.080)
    count_ok = len(onsets) == len(schedule)
    errors = []
    if count_ok:
        errors = [(onsets[n] - onsets[n - 1] - (schedule[n] - schedule[n - 1])) * 1000
                  for n in range(1, len(onsets))]
    valid = count_ok and all(abs(error) <= interval_tolerance_ms for error in errors)
    report = {
        "valid_marker_match": valid, "reference": "producer epoch plus declared delay plus exact marker sample time",
        "offset_convention": "raw mixer onset minus intended presentation; positive means later",
        "producer_delay_ms": header["delay_ms"], "producer_fps": header["fps"], "expected_cycles": cycles,
        "count": len(onsets), "expected_count": len(schedule),
        "missing_count": max(0, len(schedule) - len(onsets)),
        "extra_or_duplicate_count": max(0, len(onsets) - len(schedule)),
        "interval_error_ms": errors, "rms_threshold": threshold, "trace_rows": len(times),
        "onsets_relative_epoch_seconds": onsets, "intended_relative_epoch_seconds": schedule,
        "regions_relative_epoch": regions,
        "selection": selection,
        "timing_gate": {"requested": max_offset_ms is not None, "max_absolute_offset_ms": max_offset_ms,
                        "passed": None},
        "notes": ["No recording-start log or encoded zero is used as a time origin.",
                  "An onset labels the beginning of an approximately 1 ms RMS window, not an exact sample edge.",
                  "This measures synthetic pre-encoder mixer timing, not physical capture or full-path latency."]}
    if valid:
        offsets = [(actual - intended) * 1000 for actual, intended in zip(onsets, schedule)]
        report["raw_minus_intended_ms"] = offsets
        report["median_offset_ms"] = statistics.median(offsets)
        report["min_offset_ms"] = min(offsets)
        report["max_offset_ms"] = max(offsets)
        report["peak_to_peak_jitter_ms"] = max(offsets) - min(offsets)
        if max_offset_ms is not None:
            report["timing_gate"]["passed"] = max(map(abs, offsets)) <= max_offset_ms + 1e-9
    else:
        report["reason"] = "Exact count or full interval fingerprint failed; no subset/phase matching attempted"
    return report


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tracefile", required=True)
    parser.add_argument("--producer-log", required=True)
    parser.add_argument("--cycles", type=int, default=1)
    parser.add_argument("--max-offset-ms", type=float)
    parser.add_argument("--interval-tolerance-ms", type=float, default=40.0)
    parser.add_argument("--current-generation-window", action="store_true",
                        help="Explicitly inspect only the current fixture's epoch+delay through +(cycles*20-4)s")
    args = parser.parse_args(argv)
    try:
        with Path(args.producer_log).open(encoding="utf-8") as log:
            text = log.read(65_537)
        if len(text) > 65_536:
            raise MeasurementError("Producer log exceeds the timing-header size limit")
        header = parse_producer_log(text)
        with Path(args.tracefile).open(newline="", encoding="utf-8") as trace:
            report = analyze_rows(csv.DictReader(trace), header, cycles=args.cycles,
                                  max_offset_ms=args.max_offset_ms,
                                  interval_tolerance_ms=args.interval_tolerance_ms,
                                  current_generation_window=args.current_generation_window)
        print(json.dumps(report, indent=2, allow_nan=False))
        if not report["valid_marker_match"]:
            return 2
        return 3 if report["timing_gate"]["passed"] is False else 0
    except OSError:
        print(json.dumps({"error": "Cannot read a requested measurement input"}), file=sys.stderr)
        return 1
    except (MeasurementError, ValueError) as exc:
        print(json.dumps({"error": str(exc)}), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
