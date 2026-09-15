#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline conversion policy/fixture checks. Never captures, plays, or transports audio.

The generated WAV must be converted separately by GStreamer. This tool generates
test signals and measures results; it is not a replacement resampler or downmixer.
"""
from __future__ import annotations

import argparse
from array import array
import json
import math
from pathlib import Path
import struct
import sys

# Windows bit, name, GStreamer position index, unnormalized left/right weight.
K = math.sqrt(0.5)
POSITIONS = ((0x1, "FL", 0, 1.0, 0.0), (0x2, "FR", 1, 0.0, 1.0),
             (0x4, "FC", 2, K, K), (0x8, "LFE", 3, 0.0, 0.0),
             (0x10, "BL", 4, K, 0.0), (0x20, "BR", 5, 0.0, K),
             (0x100, "BC", 8, 0.5, 0.5), (0x200, "SL", 10, K, 0.0),
             (0x400, "SR", 11, 0.0, K))
INPUT_RATE, OUTPUT_RATE = 192000, 48000


def policy(channels: int, windows_mask: int) -> dict:
    if (type(channels) is not int or not 1 <= channels <= 8 or
            type(windows_mask) is not int or windows_mask < 0):
        raise ValueError("Invalid channel count or Windows speaker mask")
    if channels == 1 and windows_mask == 0:
        return {"channels": 1, "windows_mask": 0, "gst_mask": 0, "positions": ["MONO"],
                "matrix": [[0.9], [0.9]], "headroom_peak": 0.9, "lfe_policy": "exclude"}
    allowed = sum(item[0] for item in POSITIONS)
    if windows_mask & ~allowed or windows_mask.bit_count() != channels:
        raise ValueError("Unknown positions or mask/channel-count mismatch")
    positions = [item for item in POSITIONS if windows_mask & item[0]]
    rows = [[item[3] for item in positions], [item[4] for item in positions]]
    largest = max(sum(abs(value) for value in row) for row in rows)
    if largest == 0:
        raise ValueError("Layout has no supported full-range contribution")
    gain = 0.9 / largest
    return {"channels": channels, "windows_mask": windows_mask,
            "gst_mask": sum(1 << item[2] for item in positions),
            "positions": [item[1] for item in positions],
            "matrix": [[value * gain for value in row] for row in rows],
            "headroom_peak": 0.9, "lfe_policy": "exclude"}


def validate_policy(value: dict) -> None:
    expected = policy(value["channels"], value["windows_mask"])
    for field in ("gst_mask", "positions", "headroom_peak", "lfe_policy"):
        if value.get(field) != expected[field]:
            raise ValueError(f"Unexpected conversion policy field: {field}")
    matrix = value.get("matrix", [])
    if len(matrix) != 2 or any(len(row) != expected["channels"] for row in matrix):
        raise ValueError("Matrix must contain output rows and input columns")
    for row, reference in zip(matrix, expected["matrix"]):
        if any(not math.isfinite(coefficient) or abs(coefficient - target) > 1e-7
               for coefficient, target in zip(row, reference)):
            raise ValueError("Matrix differs from the explicit normalized policy")


def fixture_segments(config: dict) -> list[dict]:
    validate_policy(config)
    segments = [{"kind": kind, "channel": channel, "name": f"{kind}-{name}"}
                for kind in ("tone", "impulse") for channel, name in enumerate(config["positions"])]
    segments += [{"kind": "coherent", "name": "correlated-fullscale"},
                 {"kind": "passband", "name": "passband-18kHz"},
                 {"kind": "stopband", "name": "stopband-30kHz"}]
    for index, segment in enumerate(segments):
        segment["start_frame"] = index * (INPUT_RATE // 10)
        segment["frames"] = INPUT_RATE // 10
    return segments


def generate_wav(path: Path, config: dict, segments: list[dict]) -> int:
    channels = config["channels"]
    total = sum(segment["frames"] for segment in segments)
    samples = array("f", [0.0]) * (total * channels)
    for segment in segments:
        start, frames, kind = segment["start_frame"], segment["frames"], segment["kind"]
        if kind == "impulse":
            samples[(start + frames // 2) * channels + segment["channel"]] = 0.5
            continue
        frequency = 18000 if kind == "passband" else 30000 if kind == "stopband" else 1000
        amplitude = 1.0 if kind == "coherent" else 0.25
        active = range(channels) if kind == "coherent" else (segment.get("channel", 0),)
        for index in range(frames):
            sample = amplitude * math.sin(2 * math.pi * frequency * index / INPUT_RATE)
            for channel in active:
                samples[(start + index) * channels + channel] = sample
    if sys.byteorder != "little":
        samples.byteswap()
    data = samples.tobytes()
    # WAVEFORMATEXTENSIBLE/IEEE_FLOAT. The WAV mask is Windows order, not Gst bits.
    guid = struct.pack("<IHH8s", 3, 0, 0x10, b"\x80\x00\x00\xaa\x00\x38\x9b\x71")
    fmt = struct.pack("<HHIIHHHHI16s", 0xFFFE, channels, INPUT_RATE,
                      INPUT_RATE * channels * 4, channels * 4, 32, 22, 32,
                      config["windows_mask"], guid)
    with path.open("xb") as stream:
        stream.write(b"RIFF" + struct.pack("<I", 72 + len(data)) + b"WAVEfmt " + struct.pack("<I", 40) + fmt)
        stream.write(b"fact" + struct.pack("<II", 4, total))
        stream.write(b"data" + struct.pack("<I", len(data)) + data)
    return total


def analyze_samples(samples: array, config: dict, segments: list[dict]) -> dict:
    validate_policy(config)
    expected_frames = sum(segment["frames"] for segment in segments) * OUTPUT_RATE // INPUT_RATE
    if len(samples) != expected_frames * 2 or any(not math.isfinite(value) for value in samples):
        raise ValueError("Wrong output frame count or non-finite stereo PCM")
    results = []
    for segment in segments:
        start = segment["start_frame"] * OUTPUT_RATE // INPUT_RATE
        frames = segment["frames"] * OUTPUT_RATE // INPUT_RATE
        rows = [[samples[2 * index + side] for index in range(start, start + frames)] for side in (0, 1)]
        middle = [row[frames * 3 // 10:frames * 7 // 10] for row in rows]
        rms = [math.sqrt(sum(value * value for value in row) / len(row)) for row in middle]
        kind = segment["kind"]
        if kind == "impulse":
            column = segment["channel"]
            # Neighboring tone slots legitimately ring across their boundaries.
            # Check the guarded center, including the impulse, not those edges.
            passed = all(max(map(abs, row)) <= 1e-6 if config["matrix"][side][column] == 0 else
                         max(map(abs, row)) > 1e-5 and
                         abs(max(range(len(row)), key=lambda index: abs(row[index])) - len(row) // 2) <= 1
                         for side, row in enumerate(middle))
        elif kind == "stopband":
            reference = 0.25 * config["matrix"][0][0] / math.sqrt(2)
            passed = rms[0] <= reference * 10 ** (-70 / 20) and rms[1] <= 1e-6
        else:
            column = segment.get("channel", 0)
            target = ([sum(row) / math.sqrt(2) for row in config["matrix"]] if kind == "coherent" else
                      [0.25 * row[column] / math.sqrt(2) for row in config["matrix"]])
            tolerance = 0.02 if kind == "passband" else 0.002
            passed = all(abs(actual - expected) <= max(1e-6, abs(expected) * tolerance)
                         for actual, expected in zip(rms, target))
        results.append({"name": segment["name"], "rms": rms, "passed": passed})
    peak = max(map(abs, samples), default=0.0)
    return {"passed": peak < 1.0 and all(item["passed"] for item in results), "peak": peak,
            "segments": results, "scope": "Offline routing/resampling fixture only; no capture/network/A-V validation"}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create", help="Generate a private, new offline fixture directory")
    create.add_argument("directory", type=Path)
    analyze = commands.add_parser("analyze", help="Analyze separately converted F32LE/48kHz/stereo raw PCM")
    analyze.add_argument("manifest", type=Path)
    analyze.add_argument("converted", type=Path)
    args = parser.parse_args()
    if args.command == "create":
        config = policy(8, 0x63F)
        segments = fixture_segments(config)
        args.directory.mkdir(mode=0o700)
        total = generate_wav(args.directory / "input.wav", config, segments)
        manifest = {"version": 1, "policy": config, "segments": segments,
                    "input_rate": INPUT_RATE, "output_rate": OUTPUT_RATE, "input_frames": total}
        (args.directory / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        print(json.dumps({"manifest": str(args.directory / "manifest.json"), "scope": "Generated only; conversion not run"}))
        return 0
    manifest = json.loads(args.manifest.read_text(encoding="utf-8"))
    if manifest.get("version") != 1 or manifest.get("input_rate") != INPUT_RATE or manifest.get("output_rate") != OUTPUT_RATE:
        raise ValueError("Unsupported fixture manifest")
    config = manifest["policy"]
    if config != policy(8, 0x63F) or manifest["segments"] != fixture_segments(config):
        raise ValueError("Fixture policy or segment schedule differs from the generated contract")
    expected_bytes = manifest["input_frames"] * OUTPUT_RATE // INPUT_RATE * 2 * 4
    if args.converted.stat().st_size != expected_bytes or expected_bytes > 4 * 1024 * 1024:
        raise ValueError("Unexpected or excessive converted fixture size")
    samples = array("f")
    samples.frombytes(args.converted.read_bytes())
    if sys.byteorder != "little":
        samples.byteswap()
    result = analyze_samples(samples, config, manifest["segments"])
    print(json.dumps(result, indent=2, allow_nan=False))
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"Fixture error: {error}", file=sys.stderr)
        raise SystemExit(2)
