<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Decoded split-tone investigation

This note describes an anomalous **synthetic encoded test recording**, not a
physical device or a live streaming session. Its purpose is to keep the failed
result visible rather than relax the detector until it reports a pass.

## Direct observations

The desktop audio track contained seven detected regions where the known
single-cycle sequence requires six. The second intended 40 ms tone was split:

| Decoded desktop interval | Observed content |
| --- | --- |
| Approximately 1.780–1.790 s | About 10 ms of tone |
| Approximately 1.790–1.810 s | About 20 ms near silence |
| Approximately 1.810–1.842 s | About 31 ms of tone |

Multiple one-millisecond RMS windows inside that gap were exactly zero in the
decoded float PCM. Adjacent windows near its edges contained low-level codec
ringing, rather than a continuous tone hidden just below the chosen threshold.
The extra region remained present across an eightfold RMS threshold sweep,
approximately 0.0066 through 0.0528. The detector's allowed three-millisecond
hole bridging therefore did not manufacture this roughly twenty-millisecond
split. It must not be increased to conceal it.

The other audio track contained one continuous corresponding tone at
approximately 1.802–1.842 s. The desktop track's first marker was also about
21.7 ms earlier than the other track; later markers aligned between the tracks
within the measurement resolution. The encoded audio frames themselves had
ordinary container timestamp quantization residuals of at most about 0.67 ms,
not a twenty-millisecond discontinuity in the decoded frame PTS.

## What this does and does not establish

This is a real split **in the decoded recording**, not merely threshold chatter.
It does not by itself identify where the split originated. Producer scheduling,
IPC consumption, adapter sample continuity, OBS buffering/mixing, and encoding
remain distinct stages. A pre-encoder mixer trace and controlled repeat runs
are required to localize the cause. In particular, evidence from this file is
not proof that a capture device, network transport, or codec caused the gap.

AAC envelope/pre-echo, the approximately one-millisecond RMS analysis window,
and timestamp quantization limit the precision of the quoted boundaries.
Both encoded audio tracks begin with a negative timestamp and carry encoder
padding metadata. The analyzer maps PCM samples to decoded frame PTS; it does
not independently rebase tracks or subtract that padding a second time.

## Analyzer behavior

The default remains exactly one complete six-marker fingerprint. Any count
mismatch prevents offset reporting, including when a split tone would otherwise
allow a convenient subset to be paired. Region start, end, and duration values
are included in successful decode diagnostics, including count-mismatch reports.

For a controlled longer recording, explicitly request the number of complete
cycles. The cross-cycle gap is part of the expected fingerprint. For example:

```
python tools/measure_synthetic.py synthetic.mkv --cycles 3 --max-median-ms 16.667 --max-offset-ms 33.333
```

The cycle count is never inferred, and events are never matched by modulo,
arbitrary cycle shifts, or selecting a passing subset. The current bounded
decoder supports recordings up to 120 seconds and one through six complete
cycles; recording start and stop must contain that exact requested sequence.

Marker identity and timing acceptance are separate results. Optional timing
limits apply to the absolute per-track median and every absolute marker offset.
The report also includes peak-to-peak variation, first-to-last change, and each
cycle's median. Exit codes are:

- `0`: complete marker match, and timing gates pass if requested.
- `1`: invalid arguments, decoding failure, or ambiguous detection.
- `2`: marker count or interval fingerprint mismatch; timing is not evaluated.
- `3`: markers match but a requested timing limit fails.

Without explicit timing limits, a valid marker match is a measurement, not a
claim that synchronization meets a target. None of these synthetic results is
hardware or live OBS workflow validation.
