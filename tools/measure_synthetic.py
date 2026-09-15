#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure explicitly counted complete synthetic marker cycles in a test file.

Requires Python 3.10+, ffmpeg and ffprobe; no Python packages. Read-only analysis
of trusted short test recordings. This is not a hardware calibration tool.
"""

from __future__ import annotations

import argparse
from array import array
from bisect import bisect_right
import json
import math
import statistics
import subprocess
import sys
from typing import Sequence

EVENT_SECONDS = (1.0, 2.35, 4.1, 6.7, 10.15, 14.8)
CYCLE_SECONDS = 20.0
MAX_CYCLES = 6  # Bounded by the 120-second short-recording decoder limit.
GRAY_WIDTH, GRAY_HEIGHT = 32, 18


class MeasurementError(ValueError):
    pass


def finite_number(value: object, label: str) -> float:
    try:
        number = float(value)
    except (TypeError, ValueError) as exc:
        raise MeasurementError(f"Missing or invalid {label}") from exc
    if not math.isfinite(number):
        raise MeasurementError(f"Non-finite {label}")
    return number


def run(command: list[str]) -> bytes:
    try:
        process = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                 timeout=60, check=False)
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise MeasurementError(f"Cannot run {command[0]}: {exc}") from exc
    if process.returncode:
        raise MeasurementError(f"{command[0]} failed: {process.stderr.decode(errors='replace')[-2000:]}")
    return process.stdout


def probe(path: str, ffprobe: str, stream_index: int | None = None) -> dict:
    command = [ffprobe, "-v", "error"]
    if stream_index is None:
        command += ["-show_streams", "-show_format"]
    else:
        command += ["-select_streams", str(stream_index), "-show_frames", "-show_entries",
                    "frame=best_effort_timestamp_time,pts_time,nb_samples:frame_side_data"]
    try:
        return json.loads(run(command + ["-of", "json", path]))
    except json.JSONDecodeError as exc:
        raise MeasurementError("Invalid ffprobe JSON") from exc


def decoded_times(frames: list[dict]) -> list[float]:
    times = [finite_number(frame.get("best_effort_timestamp_time", frame.get("pts_time")),
                           "decoded frame timestamp") for frame in frames]
    if not times or any(b <= a for a, b in zip(times, times[1:])):
        raise MeasurementError("Decoded timestamps are missing, duplicated, or non-increasing")
    return times


def threshold_regions(times: Sequence[float], levels: Sequence[float], durations: Sequence[float],
                      threshold: float, *, bridge_gap: float = 0.003) -> list[dict]:
    """Expose every above-threshold region; never merge genuine inter-tone gaps."""
    if not (len(times) == len(levels) == len(durations)):
        raise MeasurementError("Detector arrays have different lengths")
    if not math.isfinite(threshold) or threshold <= 0 or not math.isfinite(bridge_gap) or bridge_gap < 0:
        raise MeasurementError("Invalid detector limits")
    regions: list[list[float]] = []
    previous_time = None
    for timestamp, level, duration in zip(times, levels, durations):
        if (not all(math.isfinite(x) for x in (timestamp, level, duration)) or duration <= 0 or
                (previous_time is not None and timestamp <= previous_time)):
            raise MeasurementError("Invalid detector timestamps or values")
        previous_time = timestamp
        if level < threshold:
            continue
        if regions and timestamp - regions[-1][1] <= bridge_gap + 1e-9:
            regions[-1][1] = timestamp + duration
        else:
            regions.append([timestamp, timestamp + duration])
    return [{"start_seconds": start, "end_seconds": end, "duration_ms": (end - start) * 1000}
            for start, end in regions]


def region_onsets(regions: Sequence[dict], *, min_duration: float = 0.010,
                  max_duration: float = 0.200) -> list[float]:
    if (not all(math.isfinite(value) for value in (min_duration, max_duration)) or
            min_duration <= 0 or max_duration < min_duration):
        raise MeasurementError("Invalid region duration limits")
    onsets = []
    for region in regions:
        start, end = region["start_seconds"], region["end_seconds"]
        duration = end - start
        if duration < min_duration - 1e-9:
            # A short isolated click is reported as an ambiguity, not silently
            # discarded to manufacture the expected event count.
            raise MeasurementError("Ambiguous short above-threshold region")
        if duration > max_duration + 1e-9:
            raise MeasurementError("Ambiguous long above-threshold region")
        onsets.append(start)
    return onsets


def threshold_onsets(times: Sequence[float], levels: Sequence[float], durations: Sequence[float],
                     threshold: float, *, bridge_gap: float = 0.003,
                     min_duration: float = 0.010, max_duration: float = 0.200) -> list[float]:
    regions = threshold_regions(times, levels, durations, threshold, bridge_gap=bridge_gap)
    return region_onsets(regions, min_duration=min_duration, max_duration=max_duration)


def expected_schedule(cycles: int = 1) -> tuple[float, ...]:
    if isinstance(cycles, bool) or not isinstance(cycles, int) or not 1 <= cycles <= MAX_CYCLES:
        raise MeasurementError(f"Cycle count must be an integer from 1 to {MAX_CYCLES}")
    return tuple(cycle * CYCLE_SECONDS + event for cycle in range(cycles) for event in EVENT_SECONDS)


def fingerprint(onsets: Sequence[float], tolerance: float = 0.040, *, cycles: int = 1) -> dict:
    schedule = expected_schedule(cycles)
    if not math.isfinite(tolerance) or tolerance <= 0:
        raise MeasurementError("Fingerprint tolerance must be positive")
    if any(not math.isfinite(value) for value in onsets):
        raise MeasurementError("Non-finite marker timestamp")
    count = len(onsets)
    result = {"count": count, "expected_count": len(schedule), "expected_cycles": cycles,
              "missing_count": max(0, len(schedule) - count),
              "extra_or_duplicate_count": max(0, count - len(schedule)),
              "valid": False, "interval_error_ms": []}
    if count != len(schedule):
        result["reason"] = "Marker count mismatch; no subset or cycle matching attempted"
        return result
    gaps = [b - a for a, b in zip(onsets, onsets[1:])]
    expected = [b - a for a, b in zip(schedule, schedule[1:])]
    errors = [gap - reference for gap, reference in zip(gaps, expected)]
    result["interval_error_ms"] = [error * 1000 for error in errors]
    result["valid"] = all(gap > 0 and abs(error) <= tolerance
                          for gap, error in zip(gaps, errors))
    if not result["valid"]:
        result["reason"] = "Nonuniform interval fingerprint mismatch; pairing rejected"
    return result


def compare_onsets(video: Sequence[float], audio_tracks: Sequence[Sequence[float]],
                   tolerance: float = 0.040, *, cycles: int = 1,
                   max_median_ms: float | None = None, max_offset_ms: float | None = None) -> dict:
    if len(audio_tracks) != 2:
        raise MeasurementError("Exactly two distinct audio tracks are required")
    for limit in (max_median_ms, max_offset_ms):
        if limit is not None and (not math.isfinite(limit) or limit < 0):
            raise MeasurementError("Timing limits must be finite and nonnegative")
    gate_requested = max_median_ms is not None or max_offset_ms is not None
    checks = [fingerprint(video, tolerance, cycles=cycles)] + [
        fingerprint(track, tolerance, cycles=cycles) for track in audio_tracks]
    valid = all(check["valid"] for check in checks)
    result = {"valid_marker_match": valid, "video_onsets_seconds": list(video),
              "video_fingerprint": checks[0], "tracks": [],
              "offset_convention": "audio minus video; positive means audio is later",
              "pairing": "complete ordered requested schedule; no subsets, modulo, or cycle shift",
              "expected_cycles": cycles,
              "timing_gate": {"requested": gate_requested,
                              "max_absolute_median_ms": max_median_ms,
                              "max_absolute_offset_ms": max_offset_ms,
                              "passed": None}}
    for index, track in enumerate(audio_tracks):
        report = {"audio_track": index, "onsets_seconds": list(track), "fingerprint": checks[index + 1]}
        if valid:
            offsets = [(a - v) * 1000 for a, v in zip(track, video)]
            report["audio_minus_video_ms"] = offsets
            report["median_offset_ms"] = statistics.median(offsets)
            report["min_offset_ms"] = min(offsets)
            report["max_offset_ms"] = max(offsets)
            report["peak_to_peak_jitter_ms"] = max(offsets) - min(offsets)
            report["population_stddev_ms"] = statistics.pstdev(offsets)
            report["first_to_last_offset_change_ms"] = offsets[-1] - offsets[0]
            report["cycle_median_offset_ms"] = [statistics.median(offsets[start:start + len(EVENT_SECONDS)])
                                                for start in range(0, len(offsets), len(EVENT_SECONDS))]
            if gate_requested:
                violations = []
                if max_median_ms is not None and abs(report["median_offset_ms"]) > max_median_ms + 1e-9:
                    violations.append("absolute median offset exceeds limit")
                if max_offset_ms is not None and max(map(abs, offsets)) > max_offset_ms + 1e-9:
                    violations.append("at least one absolute marker offset exceeds limit")
                report["timing_gate"] = {"passed": not violations, "violations": violations}
        result["tracks"].append(report)
    if not valid:
        result["timing_gate"]["reason"] = "Not evaluated: marker match is invalid"
    elif gate_requested:
        result["timing_gate"]["passed"] = all(track["timing_gate"]["passed"] for track in result["tracks"])
    else:
        result["timing_gate"]["reason"] = "Not requested: offsets are measurements only"
    return result


def measure_video(path: str, stream: dict, ffmpeg: str, ffprobe: str) -> tuple[list[float], dict]:
    index = int(stream["index"])
    frames = probe(path, ffprobe, index).get("frames", [])
    times = decoded_times(frames)
    raw = run([ffmpeg, "-nostdin", "-v", "error", "-copyts", "-i", path, "-map", f"0:{index}",
               "-an", "-vf", f"scale={GRAY_WIDTH}:{GRAY_HEIGHT}:flags=area,format=gray",
               "-fps_mode", "passthrough", "-f", "rawvideo", "pipe:1"])
    frame_bytes = GRAY_WIDTH * GRAY_HEIGHT
    if len(raw) != len(times) * frame_bytes:
        raise MeasurementError("Raw video frame count differs from decoded timestamp count")
    levels = [sum(raw[offset:offset + frame_bytes]) / frame_bytes
              for offset in range(0, len(raw), frame_bytes)]
    if max(levels) - min(levels) < 80:
        raise MeasurementError("Video has insufficient full-frame flash contrast")
    gaps = [b - a for a, b in zip(times, times[1:])]
    if not gaps:
        raise MeasurementError("Too few video frames")
    period = statistics.median(gaps)
    durations = gaps + [period]
    regions = threshold_regions(times, levels, durations, (min(levels) + max(levels)) / 2,
                                bridge_gap=period / 2)
    onsets = region_onsets(regions, min_duration=period / 2, max_duration=0.2)
    return onsets, {"stream_index": index, "codec": stream.get("codec_name"),
                    "stream_start_time": stream.get("start_time"), "decoded_first_pts": times[0],
                    "decoded_frames": len(times), "median_frame_interval_ms": period * 1000,
                    "max_frame_interval_ms": max(gaps) * 1000,
                    "detected_regions": regions,
                    "detector": "full-frame grayscale flash, binary pixel IDs deliberately ignored"}


def measure_audio(path: str, stream: dict, ffmpeg: str, ffprobe: str) -> tuple[list[float], dict]:
    index = int(stream["index"])
    rate = int(stream.get("sample_rate", 0))
    if not 8000 <= rate <= 192000:
        raise MeasurementError("Unsupported audio sample rate")
    frames = probe(path, ffprobe, index).get("frames", [])
    frame_times = decoded_times(frames)
    counts = [int(frame.get("nb_samples", 0)) for frame in frames]
    if any(count <= 0 or count > rate for count in counts):
        raise MeasurementError("Invalid decoded audio frame sample count")
    raw = run([ffmpeg, "-nostdin", "-v", "error", "-copyts", "-i", path, "-map", f"0:{index}",
               "-vn", "-ac", "1", "-c:a", "pcm_f32le", "-f", "f32le", "pipe:1"])
    samples = array("f")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    if len(samples) != sum(counts):
        raise MeasurementError("Decoded PCM count differs from ffprobe frames; padding/skip mapping is unsafe")
    if any(not math.isfinite(sample) for sample in samples):
        raise MeasurementError("Non-finite decoded PCM")
    starts = []
    total = 0
    for count in counts:
        starts.append(total)
        total += count
    # AAC and container timestamps may be quantized. Map each RMS window to its
    # actual decoded frame PTS, not to stream start plus a fabricated contiguous
    # sample clock. Decoder skip/padding is already reflected in frame counts.
    window = max(1, round(rate / 1000))
    times, levels, durations = [], [], []
    for position in range(0, len(samples), window):
        block = samples[position:position + window]
        frame_index = bisect_right(starts, position) - 1
        timestamp = frame_times[frame_index] + (position - starts[frame_index]) / rate
        times.append(timestamp)
        levels.append(math.sqrt(sum(value * value for value in block) / len(block)))
        durations.append(len(block) / rate)
    peak = max(levels)
    if peak < 0.005:
        raise MeasurementError("Audio contains no sufficiently strong synthetic tones")
    threshold = max(0.002, peak * 0.12)
    regions = threshold_regions(times, levels, durations, threshold, bridge_gap=0.003)
    onsets = region_onsets(regions, min_duration=0.010, max_duration=0.080)
    residuals = [frame_times[n + 1] - (frame_times[n] + counts[n] / rate)
                 for n in range(len(counts) - 1)]
    return onsets, {"stream_index": index, "codec": stream.get("codec_name"), "sample_rate": rate,
                    "stream_start_time": stream.get("start_time"), "decoded_first_pts": frame_times[0],
                    "initial_padding_metadata": stream.get("initial_padding"),
                    "decoded_frames": len(frames), "decoded_samples": len(samples),
                    "rms_window_ms": window / rate * 1000, "rms_threshold": threshold,
                    "peak_rms": peak,
                    "detected_regions": regions,
                    "max_decoded_frame_continuity_residual_ms": max(map(abs, residuals), default=0) * 1000,
                    "decoder_side_data": [frame["side_data_list"] for frame in frames if "side_data_list" in frame]}


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording", help="Short encoded synthetic recording, supplied at runtime")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--interval-tolerance-ms", type=float, default=40.0)
    parser.add_argument("--cycles", type=int, default=1,
                        help="Explicit count of complete 20-second cycles (1..6); never inferred")
    parser.add_argument("--max-median-ms", type=float,
                        help="Fail timing gate when either track's absolute median offset exceeds this")
    parser.add_argument("--max-offset-ms", type=float,
                        help="Fail timing gate when any absolute marker offset exceeds this")
    parser.add_argument("--max-duration", type=float, default=120.0)
    args = parser.parse_args(argv)
    try:
        expected_schedule(args.cycles)
        metadata = probe(args.recording, args.ffprobe)
        duration = finite_number(metadata.get("format", {}).get("duration"), "recording duration")
        if not math.isfinite(args.max_duration) or not 0 < duration <= args.max_duration <= 120:
            raise MeasurementError("Recording exceeds the short-test duration limit (maximum 120 seconds)")
        videos = [stream for stream in metadata.get("streams", []) if stream.get("codec_type") == "video"]
        audios = [stream for stream in metadata.get("streams", []) if stream.get("codec_type") == "audio"]
        if len(videos) != 1 or len(audios) != 2:
            raise MeasurementError("Require exactly one video stream and two distinct audio streams")
        video_onsets, video_info = measure_video(args.recording, videos[0], args.ffmpeg, args.ffprobe)
        measured = [measure_audio(args.recording, stream, args.ffmpeg, args.ffprobe) for stream in audios]
        result = compare_onsets(video_onsets, [item[0] for item in measured], args.interval_tolerance_ms / 1000,
                                cycles=args.cycles, max_median_ms=args.max_median_ms,
                                max_offset_ms=args.max_offset_ms)
        result["video_decode"] = video_info
        for report, (_, info) in zip(result["tracks"], measured):
            report["decode"] = info
        result["notes"] = [
            "Decoded frame PTS are used, including negative starts; no stream is independently rebased to zero.",
            "FFmpeg handles codec skip/padding when signaled. Initial-padding metadata is not subtracted a second time.",
            "AAC pre-echo/envelope and the approximately 1 ms RMS window limit onset precision; video is frame-quantized.",
            "A valid marker match is not a claim of acceptable synchronization or hardware validation.",
            "Count differences are net missing/extra detections, not proof of which physical marker was lost."]
        print(json.dumps(result, indent=2, allow_nan=False))
        if not result["valid_marker_match"]:
            return 2
        return 3 if result["timing_gate"]["passed"] is False else 0
    except (MeasurementError, KeyError, ValueError) as exc:
        print(json.dumps({"valid_marker_match": False, "error": str(exc)}, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
