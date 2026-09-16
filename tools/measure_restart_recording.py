#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Strict generated A1-A3 / B1-B6 restart recording analysis; no capture devices.

Read-only, bounded to 120 seconds. Python standard library + FFmpeg/ffprobe.
The controller separately proves marker A4 was queued and replacement finished
before it could be handed to OBS. This analyzer does not invent that boundary.
"""
from __future__ import annotations

import argparse
from array import array
from bisect import bisect_left, bisect_right
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys

import measure_physical as bounded
from measure_synthetic import MeasurementError, region_onsets, threshold_regions

RATE = 48_000
MAX_NS = (1 << 63) - 1
MAX_ROWS = 150_000
EVENT_MS = (1000, 2350, 4100, 6700, 10150, 14800)
EXPECTED_IDS = ((1, 1), (1, 2), (1, 3)) + tuple((2, n) for n in range(1, 7))
TONE_IDS = {(660 + 220 * (event - 1) + 2200 * (role - 1)): (role, event)
            for role in (1, 2) for event in range(1, 7)}
WIDTH, HEIGHT = 32, 18


def parse_header(text: str, role: int) -> dict:
    if role not in (1, 2) or len(text) > 65_536:
        raise MeasurementError("Invalid fixture role or excessive producer log")
    lines = [line for line in text.splitlines() if line.startswith("SYNTHETIC ")]
    if len(lines) != 1:
        raise MeasurementError("Each fixture requires exactly one producer header")
    pairs = re.findall(r"\b(fixture_id|epoch_ns|delay_ms|fps)=([^\s]+)", lines[0])
    fields = dict(pairs)
    if len(pairs) != 4 or len(fields) != 4:
        raise MeasurementError("Missing or duplicate producer timing fields")
    if any(not re.fullmatch(r"[0-9]+", value) for value in fields.values()):
        raise MeasurementError("Producer timing fields must be unsigned decimal integers")
    values = {key: int(value) for key, value in fields.items()}
    if (values["fixture_id"] != role or values["delay_ms"] != 2000 or values["fps"] != 60 or
            not 0 <= values["epoch_ns"] <= MAX_NS):
        raise MeasurementError("Producer fixture, delay, frame rate or clock domain is invalid")
    return values


def expected_times(predecessor: dict, successor: dict) -> list[int]:
    for header, role in ((predecessor, 1), (successor, 2)):
        if (header.get("fixture_id") != role or header.get("delay_ms") != 2000 or header.get("fps") != 60 or
                not isinstance(header.get("epoch_ns"), int) or isinstance(header["epoch_ns"], bool) or
                not 0 <= header["epoch_ns"] <= MAX_NS):
            raise MeasurementError("Invalid fixture timing header")
    if successor["epoch_ns"] <= predecessor["epoch_ns"]:
        raise MeasurementError("Successor epoch must follow predecessor epoch")
    times = []
    for role, event in EXPECTED_IDS:
        header = predecessor if role == 1 else successor
        frame = (EVENT_MS[event - 1] * header["fps"] + 500) // 1000
        sample = frame * RATE // header["fps"]
        timestamp = header["epoch_ns"] + 2_000_000_000 + sample * 1_000_000_000 // RATE
        if timestamp > MAX_NS:
            raise MeasurementError("Fixture presentation timestamp overflows")
        times.append(timestamp)
    if any(b <= a for a, b in zip(times, times[1:])) or times[-1] - times[0] > 120_000_000_000:
        raise MeasurementError("Fixture epochs do not form one ordered bounded restart timeline")
    return times


def analyze_encoded(video: list[tuple[int, float]], audio: list[tuple[int, int, float]],
                    predecessor: dict, successor: dict) -> dict:
    intended = expected_times(predecessor, successor)
    for events in (video, audio):
        times = [event[-1] for event in events]
        if any(not isinstance(t, (int, float)) or isinstance(t, bool) or not math.isfinite(t) for t in times):
            raise MeasurementError("Non-finite encoded onset")
        if any(b <= a for a, b in zip(times, times[1:])):
            raise MeasurementError("Encoded onsets are not strictly increasing")
    identities = ([event[0] for event in video] == [role for role, _ in EXPECTED_IDS] and
                  [(event[0], event[1]) for event in audio] == list(EXPECTED_IDS))
    report = {"valid_marker_match": False, "expected_event_ids": [f"{'A' if r == 1 else 'B'}{n}" for r, n in EXPECTED_IDS],
              "video_count": len(video), "audio_count": len(audio),
              "observed_audio_ids": [f"{'A' if r == 1 else 'B'}{n}" for r, n, _ in audio],
              "pairing": "exact complete A1-A3 then B1-B6; no subsets, cycles, or per-role rebasing",
              "timing_gate": {"passed": None, "max_absolute_role_median_ms": 16.667,
                              "max_absolute_event_ms": 33.333}, "roles": []}
    if not identities:
        report["reason"] = "Missing, unexpected, duplicate or reordered fixture identity"
        return report
    relative = [(stamp - intended[0]) / 1e9 for stamp in intended]
    shape_errors = [[(event[-1] - events[0][-1] - target) * 1000 for event, target in zip(events, relative)]
                    for events in (video, audio)]
    report["full_timeline_error_ms"] = {"video": shape_errors[0], "audio": shape_errors[1]}
    if any(abs(error) > 40 + 1e-8 for errors in shape_errors for error in errors):
        report["reason"] = "Full two-epoch timing fingerprint mismatch"
        return report
    report["valid_marker_match"] = True
    offsets = [(a[-1] - v[-1]) * 1000 for v, a in zip(video, audio)]
    report["audio_minus_video_ms"] = offsets
    for role, indices in (("predecessor", range(3)), ("successor", range(3, 9))):
        values = [offsets[n] for n in indices]
        median = statistics.median(values)
        report["roles"].append({"role": role, "median_audio_minus_video_ms": median,
                                "passed": abs(median) <= 16.667 + 1e-8 and
                                max(map(abs, values)) <= 33.333 + 1e-8})
    report["timing_gate"]["passed"] = all(role["passed"] for role in report["roles"])
    return report


def analyze_mixer(rows, predecessor: dict, successor: dict) -> dict:
    intended = expected_times(predecessor, successor)
    times, levels, durations = [], [], []
    first = previous = None
    for count, row in enumerate(rows):
        if count >= MAX_ROWS:
            raise MeasurementError("Mixer trace exceeds row limit")
        try:
            stamp, frames, rms = int(row["timestamp_ns"]), int(row["frames"]), float(row["rms"])
        except (KeyError, ValueError, TypeError, OverflowError) as exc:
            raise MeasurementError("Invalid mixer trace row") from exc
        if (not 0 <= stamp <= MAX_NS or not 1 <= frames <= 48 or not math.isfinite(rms) or rms < 0 or
                (previous is not None and stamp <= previous)):
            raise MeasurementError("Invalid or nonmonotonic mixer trace")
        if first is None:
            first = stamp
        if stamp - first > 130_000_000_000:
            raise MeasurementError("Mixer trace exceeds duration limit")
        previous = stamp
        times.append((stamp - predecessor["epoch_ns"]) / 1e9)
        levels.append(rms)
        durations.append(frames / RATE)
    if not levels or max(levels) < .005:
        raise MeasurementError("Mixer trace contains no sufficiently strong fixture sound")
    regions = threshold_regions(times, levels, durations, max(.002, max(levels) * .12), bridge_gap=.003)
    onsets = region_onsets(regions, min_duration=.010, max_duration=.080)
    expected = [(stamp - predecessor["epoch_ns"]) / 1e9 for stamp in intended]
    report = {"valid_marker_match": len(onsets) == 9, "count": len(onsets), "expected_count": 9,
              "selection": "entire trace; no ignored regions", "trace_rows": len(times),
              "timing_gate": {"passed": None, "max_absolute_offset_ms": 2.0}}
    if len(onsets) != 9:
        report["reason"] = "Full trace does not contain exactly nine marker regions"
        return report
    offsets = [(actual - target) * 1000 for actual, target in zip(onsets, expected)]
    report["raw_minus_intended_ms"] = offsets
    report["timing_gate"]["passed"] = max(map(abs, offsets)) <= 2.0 + 1e-8
    return report


def classify_tone(samples, rate: int = RATE) -> tuple[int, int]:
    """Identify every full RMS region independently, not its expected position."""
    if rate != RATE or not 480 <= len(samples) <= 4800 or any(not math.isfinite(x) for x in samples):
        raise MeasurementError("Invalid tone region samples")
    weighted = [sample * (.5 - .5 * math.cos(2 * math.pi * n / (len(samples) - 1)))
                for n, sample in enumerate(samples)]
    weight_sum = (len(samples) - 1) / 2
    energy = sum(sample * value for sample, value in zip(samples, weighted)) / weight_sum
    if energy < .000004:
        raise MeasurementError("Tone region is too weak to identify")
    scores = []
    for frequency, identity in TONE_IDS.items():
        omega = 2 * math.pi * frequency / rate
        real = sum(value * math.cos(omega * n) for n, value in enumerate(weighted))
        imaginary = sum(value * math.sin(omega * n) for n, value in enumerate(weighted))
        scores.append((2 * math.hypot(real, imaginary) / weight_sum, identity))
    scores.sort(reverse=True)
    amplitude, identity = scores[0]
    if amplitude * amplitude / (2 * energy) < .80 or scores[1][0] > amplitude * .20:
        raise MeasurementError("Strong audio region has unknown or mixed fixture identity")
    return identity


def classify_picture(rgb: bytes) -> tuple[float, int]:
    if len(rgb) != WIDTH * HEIGHT * 3:
        raise MeasurementError("Invalid decoded video frame size")
    white = cyan = total = 0
    for start in range(0, len(rgb), 3):
        red, green, blue = rgb[start:start + 3]
        total += max(red, green, blue)
        white += min(red, green, blue) > 180 and max(red, green, blue) - min(red, green, blue) < 35
        cyan += (green > 150 and blue > 150 and red < 95 and green - red > 75 and
                 blue - red > 75 and abs(green - blue) < 65)
    pixels = WIDTH * HEIGHT
    identity = 1 if white >= pixels * .75 else 2 if cyan >= pixels * .75 else 0
    return total / pixels, identity


def detect_video(times: list[float], raw: bytes) -> list[tuple[int, float]]:
    size = WIDTH * HEIGHT * 3
    if len(raw) != len(times) * size or len(times) < 2:
        raise MeasurementError("Video frame count differs from decoded timestamp count")
    classified = [classify_picture(raw[pos:pos + size]) for pos in range(0, len(raw), size)]
    levels, roles = zip(*classified)
    if max(levels) - min(levels) < 80:
        raise MeasurementError("Video lacks fixture flash contrast")
    gaps = [b - a for a, b in zip(times, times[1:])]
    period = statistics.median(gaps)
    # Matroska's common millisecond timebase alternates 16 and 17 ms; its
    # median can be 17 ms even though the source is exactly 60/1 fps.
    if not .015 <= period <= .018:
        raise MeasurementError("Restart fixture requires decoded 60 fps cadence")
    regions = threshold_regions(times, levels, gaps + [period], (min(levels) + max(levels)) / 2,
                                bridge_gap=period / 2)
    onsets = region_onsets(regions, min_duration=period / 2, max_duration=.2)
    events = []
    for region, onset in zip(regions, onsets):
        first, last = bisect_left(times, region["start_seconds"] - 1e-9), bisect_left(times, region["end_seconds"] - 1e-9)
        identities = set(roles[first:last])
        if len(identities) != 1 or not identities.issubset({1, 2}):
            raise MeasurementError("Bright video region has unknown or mixed fixture identity")
        events.append((identities.pop(), onset))
    return events


def detect_audio(samples, frame_times: list[float], counts: list[int], starts: list[int]) -> list[tuple[int, int, float]]:
    if not samples or len(samples) != sum(counts) or any(not math.isfinite(x) for x in samples):
        raise MeasurementError("Decoded audio samples do not match timestamps")
    positions = list(range(0, len(samples), 48))
    times, levels, durations = [], [], []
    for position in positions:
        block = samples[position:position + 48]
        frame = bisect_right(starts, position) - 1
        times.append(frame_times[frame] + (position - starts[frame]) / RATE)
        levels.append(math.sqrt(sum(x * x for x in block) / len(block)))
        durations.append(len(block) / RATE)
    if max(levels) < .005:
        raise MeasurementError("Audio contains no sufficiently strong fixture tones")
    regions = threshold_regions(times, levels, durations, max(.002, max(levels) * .12), bridge_gap=.003)
    onsets = region_onsets(regions, min_duration=.010, max_duration=.080)
    events = []
    for region, onset in zip(regions, onsets):
        first, last = bisect_left(times, region["start_seconds"] - 1e-9), bisect_left(times, region["end_seconds"] - 1e-9)
        if first == last:
            raise MeasurementError("Audio region has no samples")
        identity = classify_tone(samples[positions[first]:min(len(samples), positions[last - 1] + 48)])
        events.append((*identity, onset))
    return events


def measure_recording(path: str, ffmpeg: str, ffprobe: str):
    metadata = bounded.probe(path, ffprobe)
    duration = bounded.finite_number(metadata.get("format", {}).get("duration"), "duration")
    streams = metadata.get("streams")
    if not 0 < duration <= 120 or not isinstance(streams, list) or len(streams) > 8:
        raise MeasurementError("Unsupported recording duration or stream count")
    if any(not isinstance(stream, dict) for stream in streams):
        raise MeasurementError("Invalid recording streams")
    video = [s for s in streams if s.get("codec_type") == "video"]
    audio = [s for s in streams if s.get("codec_type") == "audio"]
    if len(video) != 1 or len(audio) not in (1, 2):
        raise MeasurementError("Require one picture stream and one or two generated audio tracks")
    for dimension in ("width", "height"):
        maximum = 3840 if dimension == "width" else 2160
        bounded.integer(video[0].get(dimension), dimension, 2, maximum)
    vindex = bounded.integer(video[0].get("index"), "video index", 0, 255)
    # Track zero is the isolated harness's desktop mixer. A synthetic secondary
    # mono track is optional and is not evidence about a real microphone.
    aindex = bounded.integer(audio[0].get("index"), "desktop audio index", 0, 255)
    if bounded.integer(audio[0].get("sample_rate"), "sample rate", RATE, RATE) != RATE:
        raise MeasurementError("Unsupported sample rate")
    vtimes = bounded.decoded_times(bounded.probe(path, ffprobe, vindex).get("frames", []), 14_401)
    vrgb = bounded.run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-copyts", "-i", path,
                        "-t", "121", "-map", f"0:{vindex}", "-an", "-vf",
                        f"scale={WIDTH}:{HEIGHT}:flags=area,format=rgb24", "-fps_mode", "passthrough",
                        "-f", "rawvideo", "pipe:1"], 14_401 * WIDTH * HEIGHT * 3)
    atimes, counts, starts, residual = bounded.audio_layout(bounded.probe(path, ffprobe, aindex).get("frames", []), RATE)
    raw = bounded.run([ffmpeg, "-nostdin", "-v", "error", "-threads", "2", "-copyts", "-i", path,
                       "-t", "121", "-map", f"0:{aindex}", "-vn", "-ac", "1", "-c:a", "pcm_f32le",
                       "-f", "f32le", "pipe:1"], 121 * RATE * 4)
    samples = array("f")
    if len(raw) % 4:
        raise MeasurementError("Decoded PCM byte count is not float-aligned")
    samples.frombytes(raw)
    if sys.byteorder != "little":
        samples.byteswap()
    return detect_video(vtimes, vrgb), detect_audio(samples, atimes, counts, starts), {
        "duration_seconds": duration, "decoded_video_frames": len(vtimes), "decoded_audio_samples": len(samples),
        "max_audio_frame_continuity_residual_ms": residual * 1000,
        "selected_audio_track": "desktop track zero", "audio_tracks_present": len(audio)}


def main(argv=None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--recording", required=True)
    parser.add_argument("--predecessor-log", required=True)
    parser.add_argument("--successor-log", required=True)
    parser.add_argument("--mixer-trace", required=True)
    parser.add_argument("--ffmpeg", default="ffmpeg")
    parser.add_argument("--ffprobe", default="ffprobe")
    args = parser.parse_args(argv)
    try:
        headers = []
        for path, role in ((args.predecessor_log, 1), (args.successor_log, 2)):
            with Path(path).open(encoding="utf-8") as file:
                headers.append(parse_header(file.read(65_537), role))
        expected_times(*headers)
        video, audio, decode = measure_recording(args.recording, args.ffmpeg, args.ffprobe)
        encoded = analyze_encoded(video, audio, *headers)
        with Path(args.mixer_trace).open(newline="", encoding="utf-8") as trace:
            raw = analyze_mixer(csv.DictReader(trace), *headers)
        passed = encoded["valid_marker_match"] and raw["valid_marker_match"] and bool(
            encoded["timing_gate"]["passed"] and raw["timing_gate"]["passed"])
        report = {"passed": passed, "scope": "isolated generated desktop restart recording, not physical calibration",
                  "encoded": encoded, "raw_desktop": raw, "decode": decode,
                  "notes": ["All strong regions are inspected; unknown or mixed identities are rejected.",
                            "No independent stream rebasing, cycle adjustment or passing subset is permitted.",
                            "AAC envelope, one-ms RMS windows and video frame quantization limit precision.",
                            "The runner separately certifies queuing and replacement timing; already submitted OBS audio is not retractable."]}
        print(json.dumps(report, indent=2, allow_nan=False))
        if not encoded["valid_marker_match"] or not raw["valid_marker_match"]:
            return 2
        return 0 if passed else 3
    except (OSError, UnicodeError):
        print(json.dumps({"passed": False, "error": "Cannot read a requested measurement input"}))
        return 1
    except bounded.MeasurementError:
        # FFmpeg errors may contain local paths; never copy them into a report.
        print(json.dumps({"passed": False, "error": "Media decoding or metadata validation failed"}))
        return 1
    except (MeasurementError, ValueError, TypeError, KeyError, OverflowError, csv.Error) as exc:
        print(json.dumps({"passed": False, "error": str(exc) if isinstance(exc, MeasurementError) else
                          "Invalid measurement data"}))
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
