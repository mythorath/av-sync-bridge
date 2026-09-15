# Sender-side RTP/RTCP correspondence check

Run `python tools/check_rtp_correspondence.py --sender-json sender-summary.json`.
It reads one bounded sender summary (or stdout containing exactly one summary),
uses only Python's standard library, and prints a JSON report. No network,
receiver, audio device, PCM recording, or OBS access is involved. Reports do not
repeat the input path, device identifiers, raw clock epochs or SSRC.

The check reconstructs the last RTP packet's shared monotonic timestamp from
the last RTCP sender report:

```
predicted_ns = last_sr_clock_32_32 * 1e9 / 2^32
             + signed_wrap(last_rtp_timestamp - last_sr_rtp_timestamp) * 1e9 / 48000
```

It compares this with `last_rtp_pts_ns` using exact rational arithmetic. The
permitted absolute error is at most one 48 kHz sample (exactly 1/48000 second).
The NTP-format field carries the project's **shared monotonic** clock convention,
not UTC; no wall-clock epoch adjustment is applied.

Missing reports, invalid integer ranges, ambiguous half-wrap differences,
differences outside the known 30-second diagnostic window, empty or oversized
PTS spans, unusable sender clock status, and reset histories are rejected.
The current summary does not associate separate SSRC fields with each last
RTP/SR record, so a reset would leave insufficient evidence that they belong to
the same generation. The tool refuses to pick a convenient subset or assume
that those records share a generation. Ordinary 32-bit wrap is supported when
the short signed difference is unambiguous.

Exit status is 0 for a valid passing comparison, 2 for malformed or insufficient
evidence, and 3 for valid evidence whose error exceeds one sample. The report
separates `valid_evidence` from `passed`.

A pass verifies only one **sender-side RTP/SR pair** after conversion and
packetization. It does not prove network delivery, receiver metadata recovery,
continuous audio, absolute clock accuracy, device timestamp accuracy, or real
A/V synchronization. Even the sender-side observation is at the output pad,
before proof of UDP delivery. Those are separate validation gates.
