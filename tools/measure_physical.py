#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Measure one complete six-event cyan-panel/unique-tone physical A/V test.

Read-only analysis of trusted recordings of at most 120 seconds and 3840x2160.
Requires Python 3.10+, FFmpeg/ffprobe and NumPy (only for media detection).
Reference: 200 ms cyan flashes and tones at 5, 11.35, 19.1, 29.7, 43.15,
59.8 seconds, respectively 660, 880, 1100, 1320, 1540, 1760 Hz. Play once.
Marker matching alone is not a synchronization pass or a hardware validation.
"""

from __future__ import annotations

import argparse
from bisect import bisect_right
import json
import math
import statistics
import subprocess
import sys
import threading
import time
from typing import Sequence

EVENT_SECONDS = (5.0, 11.35, 19.1, 29.7, 43.15, 59.8)
TONE_HZ = (660, 880, 1100, 1320, 1540, 1760)
MAX_DURATION = 120.0
MAX_VIDEO_FRAMES = 14_401
DETECT_WIDTH, DETECT_HEIGHT = 80, 45


class MeasurementError(ValueError):
    pass


def finite_number(value: object, label: str) -> float:
    try:
        result = float(value)
    except (TypeError, ValueError, OverflowError) as exc:
        raise MeasurementError(f"Missing or invalid {label}") from exc
    if isinstance(value, bool) or not math.isfinite(result):
        raise MeasurementError(f"Non-finite or invalid {label}")
    return result


def integer(value: object, label: str, low: int, high: int) -> int:
    result = finite_number(value, label)
    if not result.is_integer() or not low <= result <= high:
        raise MeasurementError(f"Invalid {label}; expected integer {low}..{high}")
    return int(result)


def run(command: list[str], max_bytes: int, *, timeout: float = 60.0) -> bytes:
    """Bound stdout, stderr, elapsed time and child lifetime, including failures."""
    if max_bytes <= 0 or not math.isfinite(timeout) or timeout <= 0:
        raise MeasurementError("Invalid subprocess limits")
    try:
        process = subprocess.Popen(command, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    except OSError as exc:
        raise MeasurementError(f"Cannot start decoder: {exc}") from exc
    outputs: list[bytearray] = [bytearray(), bytearray()]
    failures: list[str] = []
    failed = threading.Event()

    def read(pipe, destination: bytearray, limit: int) -> None:
        try:
            while chunk := pipe.read(16_384):
                if len(destination) + len(chunk) > limit:
                    failures.append("Decoder output exceeded its byte limit")
                    failed.set()
                    return
                destination.extend(chunk)
        except (OSError, ValueError) as exc:
            failures.append(f"Cannot read decoder output: {exc}")
            failed.set()

    readers = [threading.Thread(target=read, args=(pipe, output, limit), daemon=True)
               for pipe, output, limit in zip((process.stdout, process.stderr), outputs,
                                              (max_bytes, 65_536))]
    deadline = time.monotonic() + timeout
    for reader in readers:
        reader.start()
    try:
        while process.poll() is None:
            if failed.wait(0.02):
                raise MeasurementError(failures[0])
            if time.monotonic() >= deadline:
                raise MeasurementError("Decoder timed out")
        for reader in readers:
            reader.join(max(0, deadline - time.monotonic()))
        if any(reader.is_alive() for reader in readers):
            raise MeasurementError("Decoder output did not close before its deadline")
        if failures:
            raise MeasurementError(failures[0])
        if process.returncode or outputs[1]:
            # Every caller selects FFmpeg's error-only log level. Some damaged
            # inputs print decoder errors but still exit zero; do not bless them.
            raise MeasurementError("Decoder failed: " + outputs[1].decode(errors="replace")[-2000:])
        return bytes(outputs[0])
    finally:
        if process.poll() is None:
            process.kill()
        process.wait(timeout=5)
        for reader in readers:
            reader.join(5)
        process.stdout.close()
        process.stderr.close()


def probe(path: str, ffprobe: str, stream_index: int | None = None) -> dict:
    command = [ffprobe, "-v", "error", "-threads", "2"]
    if stream_index is None:
        command += ["-show_streams", "-show_format"]
    else:
        command += ["-read_intervals", "%+121", "-select_streams", str(stream_index),
                    "-show_frames", "-show_entries", "frame=best_effort_timestamp_time,pts_time,nb_samples"]
    try:
        value = json.loads(run(command + ["-of", "json", path],
                               8_000_000 if stream_index is not None else 512_000))
    except (json.JSONDecodeError, UnicodeDecodeError) as exc:
        raise MeasurementError("Invalid ffprobe JSON") from exc
    if not isinstance(value, dict):
        raise MeasurementError("Invalid ffprobe object")
    return value


def decoded_times(frames: list[dict], maximum: int) -> list[float]:
    if not isinstance(frames, list) or not 2 <= len(frames) <= maximum:
        raise MeasurementError("Decoded frame count is missing or exceeds its limit")
    if any(not isinstance(frame, dict) for frame in frames):
        raise MeasurementError("Invalid decoded frame metadata")
    times = [finite_number(frame.get("best_effort_timestamp_time", frame.get("pts_time")),
                           "decoded timestamp") for frame in frames]
    if any(b <= a for a, b in zip(times, times[1:])):
        raise MeasurementError("Decoded timestamps are duplicated or non-increasing")
    if abs(times[0]) > 30 or times[-1] - times[0] > MAX_DURATION + 1:
        raise MeasurementError("Decoded timeline is not a bounded, near-zero recording timeline")
    return times


def fingerprint(onsets: Sequence[float], tolerance: float) -> dict:
    if not math.isfinite(tolerance) or not 0 < tolerance <= 0.15:
        raise MeasurementError("Interval tolerance must be greater than 0 and at most 150 ms")
    values = [finite_number(value, "marker timestamp") for value in onsets]
    result = {"valid": False, "count": len(values), "expected_count": 6, "interval_error_ms": []}
    if len(values) != 6:
        result["reason"] = "Exactly six events required; no subsets or cycle matching attempted"
        return result
    gaps = [b - a for a, b in zip(values, values[1:])]
    expected = [b - a for a, b in zip(EVENT_SECONDS, EVENT_SECONDS[1:])]
    errors = [actual - wanted for actual, wanted in zip(gaps, expected)]
    result["interval_error_ms"] = [value * 1000 for value in errors]
    result["valid"] = all(gap > 0 and abs(error) <= tolerance + 1e-9
                          for gap, error in zip(gaps, errors))
    if not result["valid"]:
        result["reason"] = "Nonuniform interval fingerprint mismatch; pairing rejected"
    return result


def compare_onsets(video: Sequence[float], audio: Sequence[float], tolerance: float = 0.04,
                   *, max_median_ms: float | None = None, max_offset_ms: float | None = None) -> dict:
    for limit in (max_median_ms, max_offset_ms):
        if limit is not None and (not math.isfinite(limit) or limit < 0):
            raise MeasurementError("Timing limits must be finite and nonnegative")
    visual, sound = fingerprint(video, tolerance), fingerprint(audio, tolerance)
    valid = visual["valid"] and sound["valid"]
    requested = max_median_ms is not None or max_offset_ms is not None
    result = {"valid_marker_match": valid, "video_onsets_seconds": list(video),
              "audio_onsets_seconds": list(audio), "video_fingerprint": visual,
              "audio_fingerprint": sound, "events": [],
              "offset_convention": "audio minus video; positive means audio is later",
              "pairing": "one complete ordered six-event pass; no modulo, rebase or cycle shift",
              "timing_gate": {"requested": requested, "passed": None,
                              "max_absolute_median_ms": max_median_ms,
                              "max_absolute_offset_ms": max_offset_ms}}
    if not valid:
        result["timing_gate"]["reason"] = "Not evaluated: marker match is invalid"
        return result
    offsets = [(a - v) * 1000 for a, v in zip(audio, video)]
    result["events"] = [{"event": n + 1, "tone_hz": hz, "video_seconds": v,
                         "audio_seconds": a, "audio_minus_video_ms": delta}
                        for n, (hz, v, a, delta) in enumerate(zip(TONE_HZ, video, audio, offsets))]
    result.update(median_audio_minus_video_ms=statistics.median(offsets),
                  spread_ms=max(offsets) - min(offsets),
                  change_first_to_last_ms=offsets[-1] - offsets[0])
    center = statistics.mean(video)
    result["fitted_drift_ms_per_minute"] = 60 * sum(
        (v - center) * (offset - statistics.mean(offsets)) for v, offset in zip(video, offsets)
    ) / sum((v - center) ** 2 for v in video)
    if requested:
        violations = []
        if max_median_ms is not None and abs(statistics.median(offsets)) > max_median_ms + 1e-9:
            violations.append("absolute median offset exceeds limit")
        if max_offset_ms is not None and max(map(abs, offsets)) > max_offset_ms + 1e-9:
            violations.append("at least one absolute marker offset exceeds limit")
        result["timing_gate"].update(passed=not violations, violations=violations)
    else:
        result["timing_gate"]["reason"] = "Not requested: offsets are measurements only"
    return result


def regions(times: Sequence[float], mask: Sequence[bool], period: float) -> list[dict]:
    if len(times) != len(mask) or not times or not math.isfinite(period) or period <= 0:
        raise MeasurementError("Invalid region detector arrays")
    if any(not math.isfinite(value) for value in times) or any(b <= a for a, b in zip(times, times[1:])):
        raise MeasurementError("Invalid region timestamps")
    found, start = [], None
    for index, active in enumerate(mask):
        if active and start is None:
            start = index
        if start is not None and (not active or index == len(mask) - 1):
            end = times[index] if not active else times[index] + period
            found.append({"start_seconds": float(times[start]), "end_seconds": float(end),
                          "duration_ms": float(end - times[start]) * 1000})
            start = None
    return found


def region_onsets(found: list[dict], minimum: float, maximum: float) -> list[float]:
    for item in found:
        if not minimum - 1e-9 <= item["duration_ms"] / 1000 <= maximum + 1e-9:
            raise MeasurementError("Ambiguous short or long marker region; no candidates silently discarded")
    return [item["start_seconds"] for item in found]


def numpy_module():
    try:
        import numpy as np
    except ImportError as exc:
        raise MeasurementError("Media detection requires NumPy; install it explicitly before running this helper") from exc
    return np


def measure_video(path: str, stream: dict, ffmpeg: str, ffprobe: str) -> tuple[list[float], dict]:
    np = numpy_module()
    index = integer(stream.get("index"), "video stream index", 0, 255)
    times = decoded_times(probe(path, ffprobe, index).get("frames", []), MAX_VIDEO_FRAMES)
    raw = run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-copyts", "-i", path,
               "-t", "121", "-map", f"0:{index}", "-an", "-vf",
               f"scale={DETECT_WIDTH}:{DETECT_HEIGHT}:flags=area,format=rgb24",
               "-fps_mode", "passthrough", "-frames:v", str(MAX_VIDEO_FRAMES + 1),
               "-f", "rawvideo", "pipe:1"], MAX_VIDEO_FRAMES * DETECT_WIDTH * DETECT_HEIGHT * 3)
    frame_bytes = DETECT_WIDTH * DETECT_HEIGHT * 3
    if len(raw) != len(times) * frame_bytes:
        raise MeasurementError("Decoded video byte count differs from timestamp frame count")
    counts = []
    for offset in range(0, len(raw), frame_bytes):
        pixels = np.frombuffer(raw, np.uint8, frame_bytes, offset).reshape(-1, 3).astype(np.int16)
        red, green, blue = pixels[:, 0], pixels[:, 1], pixels[:, 2]
        counts.append(int(((green > 125) & (blue > 115) & (green - red > 60) &
                           (blue - red > 50) & (abs(green - blue) < 60)).sum()))
    if max(counts) < 12:
        raise MeasurementError("No clear cyan flash panel; test must be visible and unobscured")
    gaps = [b - a for a, b in zip(times, times[1:])]
    period = statistics.median(gaps)
    if not 1 / 121 - 1e-9 <= period <= 1 / 20 + 1e-9:
        raise MeasurementError("Unsupported decoded video cadence; expected 20..120 fps")
    found = regions(times, [count > max(12, max(counts) * 0.5) for count in counts], period)
    onsets = region_onsets(found, 0.1, 0.4)
    return onsets, {"stream_index": index, "decoded_first_pts": times[0], "decoded_frames": len(times),
                    "median_frame_interval_ms": period * 1000, "max_frame_interval_ms": max(gaps) * 1000,
                    "detected_regions": found, "detector": "cyan pixels after 80x45 area scaling"}


def audio_layout(frames: list[dict], rate: int) -> tuple[list[float], list[int], list[int], float]:
    times = decoded_times(frames, 200_000)
    counts = [integer(frame.get("nb_samples"), "audio frame sample count", 1, rate) for frame in frames]
    starts, total = [], 0
    for count in counts:
        starts.append(total)
        total += count
    if total > (MAX_DURATION + 1) * rate:
        raise MeasurementError("Decoded audio exceeds the sample limit")
    residual = max(abs(times[n + 1] - times[n] - counts[n] / rate) for n in range(len(times) - 1))
    # One millisecond container quantization is normal; actual gaps are not filled.
    if residual > 0.002 + 1e-9:
        raise MeasurementError("Audio timestamp discontinuity exceeds 2 ms; contiguous FFT mapping is unsafe")
    return times, counts, starts, residual


def measure_audio(path: str, stream: dict, ffmpeg: str, ffprobe: str) -> tuple[list[float], dict]:
    np = numpy_module()
    index = integer(stream.get("index"), "audio stream index", 0, 255)
    rate = integer(stream.get("sample_rate"), "sample rate", 8000, 192000)
    frames = probe(path, ffprobe, index).get("frames", [])
    frame_times, counts, starts, residual = audio_layout(frames, rate)
    raw = run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-copyts", "-i", path,
               "-t", "121", "-map", f"0:{index}", "-vn", "-ac", "1", "-c:a", "pcm_f32le",
               "-f", "f32le", "pipe:1"], int((MAX_DURATION + 1) * rate * 4))
    if len(raw) != sum(counts) * 4:
        raise MeasurementError("Decoded PCM count differs from timestamp frame sample counts")
    samples = np.frombuffer(raw, dtype="<f4")
    if not np.isfinite(samples).all():
        raise MeasurementError("Non-finite decoded PCM")
    window_size, hop = round(rate * 0.05), max(1, round(rate * 0.001))
    frame_count = 1 + (len(samples) - window_size) // hop
    if frame_count <= 0:
        raise MeasurementError("Audio is too short for tone detection")
    window = np.hanning(window_size + 1)[:-1].astype(np.float32)
    bins = np.rint(np.array(TONE_HZ) * window_size / rate).astype(int)
    amplitudes = np.empty((6, frame_count), dtype=np.float32)
    for start in range(0, frame_count, 128):
        count = min(128, frame_count - start)
        segment = samples[start * hop:(start + count - 1) * hop + window_size]
        windows = np.lib.stride_tricks.sliding_window_view(segment, window_size)[::hop]
        spectrum = np.fft.rfft(windows * window, axis=1) / window.sum()
        amplitudes[:, start:start + count] = abs(spectrum[:, bins]).T
    times = []
    for position in range(window_size // 2, window_size // 2 + frame_count * hop, hop):
        frame = bisect_right(starts, position) - 1
        times.append(frame_times[frame] + (position - starts[frame]) / rate)
    onsets, details = [], []
    for frequency, amplitude in zip(TONE_HZ, amplitudes):
        peak = float(amplitude.max())
        found = regions(times, amplitude > max(1e-5, peak * 0.5), hop / rate)
        candidates = region_onsets(found, 0.12, 0.3)
        details.append({"tone_hz": frequency, "peak_amplitude": peak, "detected_regions": found})
        if len(candidates) != 1:
            raise MeasurementError(f"Expected exactly one {frequency} Hz tone, found {len(candidates)}; no subset pairing")
        onsets.append(candidates[0])
    return onsets, {"stream_index": index, "sample_rate": rate, "decoded_first_pts": frame_times[0],
                    "decoded_samples": len(samples), "decoded_frames": len(frames),
                    "max_frame_continuity_residual_ms": residual * 1000,
                    "fft_window_ms": window_size / rate * 1000, "fft_hop_ms": hop / rate * 1000,
                    "tones": details}


def validate_metadata(metadata: dict, audio_track: int, max_duration: float) -> tuple[dict, dict]:
    duration = finite_number(metadata.get("format", {}).get("duration"), "recording duration")
    if not math.isfinite(max_duration) or not 0 < duration <= max_duration <= MAX_DURATION:
        raise MeasurementError("Recording exceeds short-test duration limit (maximum 120 seconds)")
    streams = metadata.get("streams", [])
    if not isinstance(streams, list) or len(streams) > 16 or any(not isinstance(s, dict) for s in streams):
        raise MeasurementError("Invalid or excessive stream metadata")
    videos = [stream for stream in streams if stream.get("codec_type") == "video"]
    audios = [stream for stream in streams if stream.get("codec_type") == "audio"]
    if len(videos) != 1 or isinstance(audio_track, bool) or not isinstance(audio_track, int) or not 0 <= audio_track < len(audios):
        raise MeasurementError("Require exactly one video stream and a valid zero-based audio track")
    video, audio = videos[0], audios[audio_track]
    integer(video.get("width"), "video width", 16, 3840)
    integer(video.get("height"), "video height", 16, 2160)
    integer(audio.get("sample_rate"), "sample rate", 8000, 192000)
    integer(audio.get("channels"), "audio channel count", 1, 8)
    indexes = [integer(stream.get("index"), "stream index", 0, 255) for stream in (video, audio)]
    if indexes[0] == indexes[1]:
        raise MeasurementError("Video and audio stream indexes are not distinct")
    return video, audio


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("recording")
    parser.add_argument("--audio-track", type=int, default=0, help="Zero-based selected audio track; other tracks ignored")
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    parser.add_argument("--interval-tolerance-ms", type=float, default=40)
    parser.add_argument("--max-median-ms", type=float)
    parser.add_argument("--max-offset-ms", type=float)
    parser.add_argument("--max-duration", type=float, default=MAX_DURATION)
    args = parser.parse_args(argv)
    try:
        # Reject invalid gates before doing expensive media decoding.
        compare_onsets([], [], args.interval_tolerance_ms / 1000,
                       max_median_ms=args.max_median_ms, max_offset_ms=args.max_offset_ms)
        video, audio = validate_metadata(probe(args.recording, args.ffprobe), args.audio_track, args.max_duration)
        visual, video_info = measure_video(args.recording, video, args.ffmpeg, args.ffprobe)
        sound, audio_info = measure_audio(args.recording, audio, args.ffmpeg, args.ffprobe)
        result = compare_onsets(visual, sound, args.interval_tolerance_ms / 1000,
                                max_median_ms=args.max_median_ms, max_offset_ms=args.max_offset_ms)
        result.update(video_decode=video_info, audio_decode=audio_info, audio_track_zero_based=args.audio_track)
        result["notes"] = [
            "Decoded PTS retained, including negative starts; no per-stream rebase, gap filling or asynchronous resampling.",
            "FFmpeg decoder skip/padding is reflected in frame sample counts; no second padding subtraction.",
            "Video is frame-quantized; tone onset uses a 50 ms Hann window with approximately 1 ms hops and a half-peak threshold.",
            "A valid marker match alone is not an acceptable-sync gate or proof of physical hardware validation."]
        print(json.dumps(result, indent=2, allow_nan=False))
        if not result["valid_marker_match"]:
            return 2
        return 3 if result["timing_gate"]["passed"] is False else 0
    except (MeasurementError, KeyError, TypeError, ValueError, OverflowError) as exc:
        print(json.dumps({"valid_marker_match": False, "error": str(exc)}, indent=2), file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
