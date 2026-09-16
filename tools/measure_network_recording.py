#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded generated network audio recording analysis, NOT video synchronization.

Checks complete tone identity/order and original absolute mixer presentation time.
One baseline contains A1..A6; a restart contains A1..A3 then B1..B6. Manifest
records mean generated samples were pushed, not that the receiver queued them.
"""
from __future__ import annotations

import argparse
from array import array
import csv
import json
import math
from pathlib import Path
import re
import sys

import measure_physical as bounded
from measure_restart_recording import detect_audio
from measure_synthetic import MeasurementError, region_onsets, threshold_regions

RATE = 48000
MAX_NS = (1 << 63) - 1
MAX_LOG = 65536
MAX_ROWS = 150000
MAX_TRACE_BYTES = 16 * 1024 * 1024
PREFIX = "AVSYNC_FIXTURE "
OFFSETS = [48000, 112800, 196800, 321600, 487200, 710400]
DELAY_NS = 2_000_000_000


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise ValueError("duplicate_key")
        value[key] = item
    return value


def decimal(value, maximum=MAX_NS):
    if (not isinstance(value, str) or not re.fullmatch(r"[1-9][0-9]{0,19}", value)
            or int(value) > maximum):
        raise MeasurementError("Invalid bounded decimal identity or timestamp")
    return int(value)


def parse_manifest(text: str, role: int, count: int) -> dict:
    if (type(role) is not int or type(count) is not int or (role, count) not in ((1, 3), (1, 6), (2, 6))
            or not isinstance(text, str) or len(text) > MAX_LOG):
        raise MeasurementError("Invalid manifest role, count or size")
    records = []
    for line in text.splitlines():
        if not line.startswith(PREFIX):
            continue
        if len(line) > 2048 or len(records) >= 7:
            raise MeasurementError("Manifest record bound exceeded")
        try:
            value = json.loads(line[len(PREFIX):], object_pairs_hook=unique_object,
                               parse_constant=lambda _: (_ for _ in ()).throw(ValueError("nonfinite")))
        except (UnicodeError, ValueError, RecursionError):
            raise MeasurementError("Invalid manifest JSON") from None
        if not isinstance(value, dict):
            raise MeasurementError("Invalid manifest object")
        records.append(value)
    if len(records) != count + 1:
        raise MeasurementError("Missing, duplicate or extra manifest records")
    header, events = records[0], records[1:]
    keys = {"schema", "type", "fixture_id", "sender_session", "clock_epoch", "generation",
            "sample_rate", "channels", "warmup_frames", "duration_frames", "capture_origin_ns", "frame_offsets"}
    if set(header) != keys or header.get("type") != "header":
        raise MeasurementError("Invalid manifest header fields")
    for key, expected in (("schema", 1), ("fixture_id", role), ("sample_rate", RATE), ("channels", 2),
                          ("warmup_frames", 480000), ("duration_frames", 1440)):
        if type(header[key]) is not int or header[key] != expected:
            raise MeasurementError("Invalid manifest header value")
    if (header["frame_offsets"] != OFFSETS or not isinstance(header["frame_offsets"], list)
            or any(type(v) is not int for v in header["frame_offsets"])):
        raise MeasurementError("Invalid nonuniform marker schedule")
    for key in ("sender_session", "clock_epoch", "generation"):
        decimal(header[key], (1 << 64) - 1)
    origin = decimal(header["capture_origin_ns"])
    previous = origin
    for number, marker in enumerate(events, 1):
        if set(marker) != {"schema", "type", "fixture_id", "event", "frame_position",
                           "capture_ns", "duration_frames", "frequency_hz"} or marker.get("type") != "marker":
            raise MeasurementError("Invalid manifest marker fields")
        for key, expected in (("schema", 1), ("fixture_id", role), ("event", number),
                              ("frame_position", 480000 + OFFSETS[number - 1]), ("duration_frames", 1440),
                              ("frequency_hz", 660 + 220 * (number - 1) + 2200 * (role - 1))):
            if type(marker[key]) is not int or marker[key] != expected:
                raise MeasurementError("Invalid manifest marker value or order")
        capture = decimal(marker["capture_ns"])
        nominal = marker["frame_position"] * 1_000_000_000 // RATE
        # Each once-mapped timestamp is authoritative. This broad sanity bound
        # does not pretend that the initial network calibration is immutable.
        if (capture <= previous or capture + DELAY_NS > MAX_NS
                or abs(capture - origin - nominal) > 100_000_000):
            raise MeasurementError("Manifest capture timeline is inconsistent or overflowing")
        previous = capture
    return {"header": header, "events": events}


def expected_events(before: dict, after: dict | None = None) -> list[tuple[int, int, int]]:
    manifests = [before] if after is None else [before, after]
    result = []
    for role, manifest in enumerate(manifests, 1):
        # Revalidate callers' dictionaries, not only text loaded by the CLI.
        try:
            lines = [manifest["header"], *manifest["events"]]
            checked = parse_manifest("\n".join(PREFIX + json.dumps(v, allow_nan=False) for v in lines),
                                     role, 3 if after is not None and role == 1 else 6)
        except (KeyError, TypeError, ValueError, RecursionError):
            raise MeasurementError("Invalid parsed manifest") from None
        for marker in checked["events"]:
            result.append((role, marker["event"], int(marker["capture_ns"]) + DELAY_NS))
    if after is not None:
        if any(before["header"][key] == after["header"][key] for key in ("sender_session", "clock_epoch")):
            raise MeasurementError("Restart requires fresh sender and provider identities")
        if int(after["header"]["capture_origin_ns"]) <= int(before["events"][-1]["capture_ns"]):
            raise MeasurementError("Successor must start after predecessor markers")
    if any(b[2] <= a[2] for a, b in zip(result, result[1:])) or result[-1][2] - result[0][2] > 120_000_000_000:
        raise MeasurementError("Invalid complete manifest timeline")
    return result


def validate_expected(expected):
    identities = [(1, n) for n in range(1, 7)]
    restart = [(1, n) for n in range(1, 4)] + [(2, n) for n in range(1, 7)]
    if (not isinstance(expected, list) or len(expected) not in (6, 9)
            or any(not isinstance(e, (tuple, list)) or len(e) != 3 or any(type(v) is not int for v in e)
                   for e in expected)
            or [tuple(e[:2]) for e in expected] not in (identities, restart)
            or any(not DELAY_NS < e[2] <= MAX_NS for e in expected)
            or any(b[2] <= a[2] for a, b in zip(expected, expected[1:]))
            or expected[-1][2] - expected[0][2] > 120_000_000_000):
        raise MeasurementError("Invalid expected event sequence")


def analyze_encoded(audio: list[tuple[int, int, float]], expected) -> dict:
    validate_expected(expected)
    for event in audio:
        if (not isinstance(event, (tuple, list)) or len(event) != 3
                or type(event[0]) is not int or type(event[1]) is not int
                or type(event[2]) not in (float, int) or not math.isfinite(event[2])):
            raise MeasurementError("Invalid encoded audio event")
    if any(b[2] <= a[2] for a, b in zip(audio, audio[1:])):
        raise MeasurementError("Encoded audio is not strictly ordered")
    report = {"valid_marker_match": False, "count": len(audio), "expected_count": len(expected),
              "observed_ids": [f"{'A' if r == 1 else 'B' if r == 2 else '?'}{n}" for r, n, _ in audio],
              "timing_gate": {"passed": None, "max_complete_timeline_error_ms": 40.0},
              "selection": "all strong regions; one common encoded origin, never per-role rebasing"}
    if [(r, e) for r, e, _ in audio] != [(r, e) for r, e, _ in expected]:
        report["reason"] = "Missing, extra, stale, duplicate or reordered marker identity"
        return report
    errors = [(observed[2] - audio[0][2] - (target[2] - expected[0][2]) / 1e9) * 1000
              for observed, target in zip(audio, expected)]
    report.update(valid_marker_match=True, full_timeline_error_ms=errors)
    report["timing_gate"]["passed"] = max(map(abs, errors)) <= 40.0 + 1e-8
    return report


def analyze_mixer(rows, expected) -> dict:
    validate_expected(expected)
    origin = expected[0][2]
    times, levels, durations = [], [], []
    first = previous = previous_frames = None
    for count, row in enumerate(rows):
        if count >= MAX_ROWS:
            raise MeasurementError("Mixer trace exceeds row limit")
        try:
            for key in ("timestamp_ns", "frames"):
                value = row[key]
                if type(value) is not int and (not isinstance(value, str)
                        or not re.fullmatch(r"(?:0|[1-9][0-9]{0,19})", value)):
                    raise ValueError("noninteger")
            if isinstance(row["rms"], bool) or (isinstance(row["rms"], str) and len(row["rms"]) > 64):
                raise ValueError("invalid_level")
            stamp, frames, rms = int(row["timestamp_ns"]), int(row["frames"]), float(row["rms"])
        except (KeyError, ValueError, TypeError, OverflowError):
            raise MeasurementError("Invalid mixer trace row") from None
        if (not 0 <= stamp <= MAX_NS or not 1 <= frames <= 48 or not math.isfinite(rms) or rms < 0
                or (previous is not None and stamp <= previous)):
            raise MeasurementError("Invalid or nonmonotonic mixer trace")
        if previous is not None and abs(stamp - previous - previous_frames * 1_000_000_000 // RATE) > 2:
            raise MeasurementError("Mixer trace contains a gap or overlap")
        first = stamp if first is None else first
        if stamp - first > 130_000_000_000:
            raise MeasurementError("Mixer trace exceeds duration limit")
        previous, previous_frames = stamp, frames
        times.append((stamp - origin) / 1e9)
        levels.append(rms)
        durations.append(frames / RATE)
    if not levels or max(levels) < .005:
        raise MeasurementError("Mixer trace contains no sufficiently strong fixture sound")
    if (first > expected[0][2] - 10_000_000
            or previous + previous_frames * 1_000_000_000 // RATE < expected[-1][2] + 40_000_000):
        raise MeasurementError("Mixer trace does not cover the complete marker span with silence margins")
    regions = threshold_regions(times, levels, durations, max(.002, max(levels) * .12), bridge_gap=.003)
    onsets = region_onsets(regions, min_duration=.010, max_duration=.080)
    report = {"valid_marker_match": len(onsets) == len(expected), "count": len(onsets),
              "expected_count": len(expected), "selection": "entire absolute-time trace; no ignored regions",
              "timing_gate": {"passed": None, "max_absolute_offset_ms": 2.0}}
    if len(onsets) != len(expected):
        report["reason"] = "Missing or extra strong mixer regions"
        return report
    offsets = [(onset - (event[2] - origin) / 1e9) * 1000 for onset, event in zip(onsets, expected)]
    report["raw_minus_intended_ms"] = offsets
    report["timing_gate"]["passed"] = max(map(abs, offsets)) <= 2.0 + 1e-8
    return report


def read_mixer_rows(path) -> list[dict[str, str]]:
    # This is the recorder's deliberately simple machine format, not arbitrary
    # user CSV. Bound bytes and lines before a parser can allocate a huge row.
    with Path(path).open("rb") as source:
        raw = source.read(MAX_TRACE_BYTES + 1)
    if len(raw) > MAX_TRACE_BYTES:
        raise MeasurementError("Mixer trace exceeds byte limit")
    try:
        text = raw.decode("ascii").replace("\r\n", "\n")
    except UnicodeError:
        raise MeasurementError("Mixer trace must be ASCII") from None
    if not text.endswith("\n") or any(ord(c) < 32 and c != "\n" for c in text):
        raise MeasurementError("Invalid mixer trace line endings")
    lines = text.split("\n")[:-1]
    if not lines or lines[0] != "timestamp_ns,frames,rms" or len(lines) > MAX_ROWS + 1:
        raise MeasurementError("Invalid mixer trace header or row count")
    result = []
    for line in lines[1:]:
        parts = line.split(",")
        if len(line) > 128 or len(parts) != 3 or any(not part or '"' in part for part in parts):
            raise MeasurementError("Invalid bounded mixer trace row")
        if any(not re.fullmatch(r"(?:0|[1-9][0-9]{0,19})", part) for part in parts[:2]):
            raise MeasurementError("Invalid mixer trace integer")
        result.append(dict(zip(("timestamp_ns", "frames", "rms"), parts)))
    return result


def measure_audio(path: str, ffmpeg: str, ffprobe: str):
    metadata = bounded.probe(path, ffprobe)
    duration = bounded.finite_number(metadata.get("format", {}).get("duration"), "duration")
    streams = metadata.get("streams")
    if (not 0 < duration <= 120 or not isinstance(streams, list) or not 1 <= len(streams) <= 8
            or any(not isinstance(stream, dict) for stream in streams)):
        raise MeasurementError("Unsupported recording duration or stream count")
    audio = [stream for stream in streams if stream.get("codec_type") == "audio"]
    if not 1 <= len(audio) <= 2:
        raise MeasurementError("Require desktop track zero and at most one unused secondary track")
    index = bounded.integer(audio[0].get("index"), "desktop index", 0, 255)
    bounded.integer(audio[0].get("sample_rate"), "sample rate", RATE, RATE)
    times, counts, starts, residual = bounded.audio_layout(
        bounded.probe(path, ffprobe, index).get("frames", []), RATE)
    raw = bounded.run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-copyts", "-i", path,
                       "-t", "121", "-map", f"0:{index}", "-vn", "-ac", "1", "-c:a", "pcm_f32le",
                       "-f", "f32le", "pipe:1"], 121 * RATE * 4)
    if len(raw) != sum(counts) * 4:
        raise MeasurementError("Decoded sample count differs from timestamps")
    samples = array("f")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    return detect_audio(samples, times, counts, starts), {"duration_seconds": duration,
        "decoded_audio_samples": len(samples), "max_audio_frame_continuity_residual_ms": residual * 1000,
        "selected_audio_track": "desktop track zero", "audio_tracks_present": len(audio), "video_analyzed": False}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recording", required=True)
    parser.add_argument("--predecessor-log", required=True)
    parser.add_argument("--successor-log")
    parser.add_argument("--mixer-trace", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args(argv)
    try:
        with Path(args.predecessor_log).open(encoding="utf-8") as source:
            before = parse_manifest(source.read(MAX_LOG + 1), 1, 3 if args.successor_log else 6)
        after = None
        if args.successor_log:
            with Path(args.successor_log).open(encoding="utf-8") as source:
                after = parse_manifest(source.read(MAX_LOG + 1), 2, 6)
        expected = expected_events(before, after)
        audio, decode = measure_audio(args.recording, args.ffmpeg, args.ffprobe)
        encoded = analyze_encoded(audio, expected)
        mixer = analyze_mixer(read_mixer_rows(args.mixer_trace), expected)
        passed = bool(encoded["valid_marker_match"] and mixer["valid_marker_match"]
                      and encoded["timing_gate"]["passed"] and mixer["timing_gate"]["passed"])
        print(json.dumps({"passed": passed, "scope": "generated network desktop audio only, NOT A/V or physical calibration",
            "encoded": encoded, "raw_desktop": mixer, "decode": decode,
            "queued_stale_replay_verified": False,
            "notes": ["Manifest push is not receiver publication proof.",
                      "Absolute mixer timing is required; encoded relative timing alone cannot prove latency.",
                      "One-ms RMS windows and AAC envelopes limit precision."]}, allow_nan=False, indent=2))
        return 0 if passed else 3
    except (OSError, UnicodeError, csv.Error):
        error = "Cannot read bounded measurement inputs"
    except bounded.MeasurementError:
        error = "Media decoding or metadata validation failed"
    except (MeasurementError, ValueError, TypeError, KeyError, OverflowError, RecursionError):
        error = "Invalid generated audio measurement data"
    print(json.dumps({"passed": False, "error": error}))
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
