#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Check one bounded sender summary's RTP/RTCP capture-time correspondence.

No network, PCM, media decoding or receiver access. Report values are relative;
input paths, raw clocks and device identifiers are not echoed. One sample is an
exact rational tolerance, not a rounded millisecond threshold.
"""

from __future__ import annotations

import argparse
from fractions import Fraction
import json
from pathlib import Path
import sys

SAMPLE_RATE = 48_000
NS_PER_SECOND = 1_000_000_000
MAX_SIGNED_NS = (1 << 63) - 1
MAX_UINT64 = (1 << 64) - 1
MAX_UINT32 = (1 << 32) - 1
MAX_CAPTURE_SECONDS = 30
MAX_JSON_BYTES = 2 * 1024 * 1024
TOLERANCE_NS = Fraction(NS_PER_SECOND, SAMPLE_RATE)


class EvidenceError(ValueError):
    """The available summary cannot safely support this comparison."""


def integer(data: dict, name: str, minimum: int, maximum: int) -> int:
    value = data.get(name)
    if type(value) is not int or not minimum <= value <= maximum:
        raise EvidenceError(f"Missing or out-of-range integer field: {name}")
    return value


def signed_rtp_delta(last_timestamp: int, sr_timestamp: int) -> int:
    for value in (last_timestamp, sr_timestamp):
        if type(value) is not int or not 0 <= value <= MAX_UINT32:
            raise EvidenceError("RTP timestamp must be an unsigned 32-bit integer")
    wrapped = (last_timestamp - sr_timestamp) & MAX_UINT32
    if wrapped == 1 << 31:
        raise EvidenceError("Exactly half a wrap has ambiguous RTP direction")
    delta = wrapped if wrapped < (1 << 31) else wrapped - (1 << 32)
    # This checker targets the bounded <=30-second diagnostic, not an arbitrary
    # long-running stream whose wrap count cannot be inferred from one summary.
    if abs(delta) > MAX_CAPTURE_SECONDS * SAMPLE_RATE:
        raise EvidenceError("RTP delta exceeds the bounded diagnostic window")
    return delta


def check_summary(summary: dict) -> dict:
    if not isinstance(summary, dict):
        raise EvidenceError("Require one sender summary object")
    if integer(summary, "schema", 1, 1) != 1 or summary.get("desktop_only") is not True:
        raise EvidenceError("Unrecognized sender summary contract")
    if summary.get("status") != "rtp_output_observed_unverified":
        raise EvidenceError("Sender did not finish with RTP output observed")
    if summary.get("clock_usable") is not True:
        raise EvidenceError("Sender clock was not usable at summary time")
    # The existing schema aggregates counters across resets and does not record
    # each last RTP/SR's SSRC. Do not infer that those two records share an epoch.
    if integer(summary, "resets", 0, MAX_UINT64) != 0:
        raise EvidenceError("Reset summary cannot prove the RTP and SR share a generation")
    packet_count = integer(summary, "rtp_packets_at_output", 2, MAX_UINT64)
    sr_count = integer(summary, "sender_reports", 1, MAX_UINT64)
    rtcp_count = integer(summary, "rtcp_packets_at_output", 1, MAX_UINT64)
    if sr_count > rtcp_count:
        raise EvidenceError("Sender-report count exceeds observed RTCP packet count")
    integer(summary, "mapped_packets", 1, MAX_UINT64)
    first_pts = integer(summary, "first_rtp_pts_ns", 1, MAX_SIGNED_NS)
    last_pts = integer(summary, "last_rtp_pts_ns", 1, MAX_SIGNED_NS)
    if not 0 < last_pts - first_pts <= MAX_CAPTURE_SECONDS * NS_PER_SECOND:
        raise EvidenceError("RTP PTS span is empty, backwards or outside the bounded run")
    sr_clock = integer(summary, "last_sr_clock_32_32", 1, MAX_UINT64)
    rtp = integer(summary, "last_rtp_timestamp", 0, MAX_UINT32)
    sr_rtp = integer(summary, "last_sr_rtp_timestamp", 0, MAX_UINT32)
    delta = signed_rtp_delta(rtp, sr_rtp)
    sr_ns = Fraction(sr_clock * NS_PER_SECOND, 1 << 32)
    # An SR may be generated just after the last media packet. Allow the whole
    # known run on either side but not an unverified clock era/cycle assignment.
    if not first_pts - MAX_CAPTURE_SECONDS * NS_PER_SECOND <= sr_ns <= last_pts + MAX_CAPTURE_SECONDS * NS_PER_SECOND:
        raise EvidenceError("Sender-report clock is outside the known diagnostic time window")
    predicted_ns = sr_ns + Fraction(delta * NS_PER_SECOND, SAMPLE_RATE)
    if not 0 <= predicted_ns <= MAX_SIGNED_NS:
        raise EvidenceError("Reconstructed clock is outside the signed nanosecond domain")
    error_ns = predicted_ns - last_pts
    passed = abs(error_ns) <= TOLERANCE_NS
    return {
        "schema": 1,
        "check": "sender_rtp_rtcp_capture_correspondence",
        "valid_evidence": True,
        "passed": passed,
        "scope": "one last sender-side RTP/SR pair; no receiver or hardware proof",
        "reference_domain": "shared_monotonic_not_utc",
        "sample_rate": SAMPLE_RATE,
        "rtp_packets_observed": packet_count,
        "sender_reports_observed": sr_count,
        "rtp_delta_samples": delta,
        "rtp_pts_span_ms": (last_pts - first_pts) / 1_000_000,
        "sr_to_last_packet_ms": float(Fraction(delta * 1000, SAMPLE_RATE)),
        "reconstructed_minus_rtp_pts_ns": float(error_ns),
        "reconstructed_minus_rtp_pts_samples": float(error_ns / TOLERANCE_NS),
        "exact_error_ns": {"numerator": error_ns.numerator, "denominator": error_ns.denominator},
        "tolerance_samples": 1,
        "tolerance_ns": float(TOLERANCE_NS),
        "receiver_verified": False,
        "clock_accuracy_verified": False,
        "continuity_verified": False,
    }


def unique_object(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise EvidenceError("Duplicate JSON object keys are ambiguous")
        result[key] = value
    return result


def invalid_constant(_: str) -> None:
    raise EvidenceError("Non-finite JSON numbers are invalid")


def parse_sender_json(text: str) -> dict:
    def decode(value: str) -> object:
        try:
            decoded = json.loads(value, object_pairs_hook=unique_object, parse_constant=invalid_constant)
        except (EvidenceError, json.JSONDecodeError):
            raise
        except (RecursionError, ValueError) as error:
            raise EvidenceError("JSON exceeds supported parser complexity") from error
        pending = [(decoded, 0)]
        visited = 0
        while pending:
            item, depth = pending.pop()
            visited += 1
            if depth > 64 or visited > 100_000:
                raise EvidenceError("JSON exceeds supported parser complexity")
            if isinstance(item, dict):
                pending.extend((child, depth + 1) for child in item.values())
            elif isinstance(item, list):
                pending.extend((child, depth + 1) for child in item)
        return decoded

    try:
        value = decode(text)
    except json.JSONDecodeError:
        # Native sender stdout can have preceding diagnostic lines. Require one
        # complete, recognizable summary; never pick the latest/best of many.
        candidates = []
        for line in text.splitlines():
            line = line.strip()
            if not line.startswith("{"):
                continue
            try:
                candidate = decode(line)
            except json.JSONDecodeError as error:
                raise EvidenceError("Malformed JSON line in sender output") from error
            if isinstance(candidate, dict) and candidate.get("desktop_only") is True:
                candidates.append(candidate)
        if len(candidates) != 1:
            raise EvidenceError("Require exactly one sender summary; no subset selection")
        return candidates[0]
    if not isinstance(value, dict):
        raise EvidenceError("Require one sender summary object")
    return value


def read_summary(path: Path) -> dict:
    with path.open("rb") as source:
        content = source.read(MAX_JSON_BYTES + 1)
    if len(content) > MAX_JSON_BYTES:
        raise EvidenceError("Sender summary exceeds the bounded input size")
    encoding = "utf-16" if content.startswith((b"\xff\xfe", b"\xfe\xff")) else "utf-8-sig"
    try:
        return parse_sender_json(content.decode(encoding))
    except UnicodeError as error:
        raise EvidenceError("Sender summary must be UTF-8 or BOM-marked UTF-16") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sender-json", required=True, type=Path, help="One sender JSON summary or stdout log")
    args = parser.parse_args(argv)
    try:
        report = check_summary(read_summary(args.sender_json))
    except (EvidenceError, OSError) as error:
        # OSError text can contain a private input path; keep it out of reports.
        reason = str(error) if isinstance(error, EvidenceError) else "Cannot read sender summary"
        print(json.dumps({"schema": 1, "valid_evidence": False, "passed": False,
                          "reason": reason, "receiver_verified": False}, sort_keys=True))
        return 2
    print(json.dumps(report, sort_keys=True, allow_nan=False))
    return 0 if report["passed"] else 3


if __name__ == "__main__":
    sys.exit(main())
