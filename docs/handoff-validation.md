<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Synthetic audio handoff validation: 2026-09-15

These are **synthetic pre-encoder OBS mixer measurements** from three old-control
recordings and three revised baseline recordings, followed by controlled recovery
fixtures and a six-cycle run. They do not establish physical-device/network sync,
system restart reliability, real-microphone privacy, or long-run performance.

## Reference and method

Each producer log supplies its shared monotonic epoch, configured 2000 ms
presentation delay, and 60 fps synthetic marker schedule. The intended audio
onsets are that epoch plus the delay plus the known marker sample position.
The raw OBS mixer callback logs approximately one-millisecond RMS windows with
the callback's audio timestamp plus the sample offset of each window.

For each track independently, threshold detection uses 12% of its peak RMS
(with a fixed minimum), permits only three-millisecond envelope holes, and
requires exactly the complete six-marker nonuniform fingerprint. No marker,
cycle, or subset alignment is optimized to obtain a result. The measurements
below are **observed raw onset minus intended presentation time**; positive
means later. An onset is the start of a window containing above-threshold sound,
so a small negative value is possible without sound actually preceding the
intended first sample. The roughly one-millisecond window limits precision.

The log message printed after recording starts is **not the muxer's timestamp
zero**. In the old controls, subtracting that log time from raw timestamps
produced apparent raw-versus-encoded offsets that differed by run: roughly
38.5 ms, 30.3 ms, and 15.4 ms. Those are not encoder-delay measurements. Encoded
A/V offsets must instead compare streams within their encoded timestamp domain;
raw absolute checks use the producer epoch. Never mix the two origins.

## Direct raw-mixer observations

Values are milliseconds, rounded to three decimals. Both tracks detected all
six continuous marker regions in every run.

| Configuration / run | Track | Minimum offset | Maximum offset | Notable behavior |
| --- | --- | ---: | ---: | --- |
| Old control, 1 | Desktop and mic | +20.844 | +21.510 | Track onsets identical |
| Old control, 2 | Desktop and mic | +20.755 | +21.422 | Track onsets identical |
| Old control, 3 | Desktop | -0.373 | +21.627 | First marker near zero; later markers about +21 ms |
| Old control, 3 | Mic | +20.961 | +21.627 | All markers about +21 ms |
| Revised 40 ms handoff, 1 | Desktop and mic | -0.553 | +0.114 | Track onsets identical |
| Revised 40 ms handoff, 2 | Desktop and mic | -0.417 | +0.250 | Track onsets identical |
| Revised 40 ms handoff, 3 | Desktop and mic | -0.459 | +0.208 | Track onsets identical |

Across the revised baselines, every one of the 18 marker observations per track
was within -0.553 to +0.250 ms of the intended presentation timestamp. That is
consistent with the detector's one-millisecond window resolution; it is not a
claim of sub-millisecond physical accuracy.

The old third control's desktop-only phase change exists in the raw mixer trace,
before AAC encoding. Its approximately 21.33 ms magnitude matches 1024 samples
at 48 kHz, but that numerical match does not prove the internal cause. These
three old controls did not reproduce the split-tone anomaly documented
separately; their detected tones remained approximately 39.3–40.7 ms long.

The revised implementation changes both the short OBS handoff lead and sample
continuity handling. This comparison therefore supports the combined revision
under the measured synthetic conditions, not a claim that the lead setting
alone caused the improvement.

## Reproducible raw-trace check

```
python tools/measure_mixer_trace.py --tracefile synthetic.mkv.mix0.csv --producer-log producer.log --max-offset-ms 2
```

Run separately for each track. `--cycles N` requires exactly N complete marker
cycles, including inter-cycle spacing. The helper mirrors the producer's integer
frame/sample rounding, emits only relative times, and refuses mixed-session
producer logs. It does not print paths, raw absolute epochs, or generations.

The proposed raw-trace gate is an absolute 2 ms limit, allowing room for the
approximately one-millisecond RMS analysis window. It rejects the measured
old-control offsets while accepting the three revised baselines.

For a controlled restart fixture whose trace also contains earlier generations,
the optional `--current-generation-window` flag selects a fixed, declared window:
from the current producer's epoch plus its delay, through that instant plus
`cycles * 20 - 4` seconds (16 seconds for one cycle). This is fixture-based
generation scoping, not a search for the best subset. The report counts ignored
prior and later regions; all extra regions inside the window still fail, and
any region straddling its boundary is rejected. Default analysis remains strict
over the entire trace. Intentionally muted-marker tests require a separate
privacy assertion and are not made to pass by this full-sequence helper.

The helper bounds logs, trace rows, and duration. Exit status `2` means the
marker sequence did not match; `3` means a requested absolute offset gate failed;
`1` means invalid input or ambiguous detection. A zero status without a timing
limit means a valid measurement, not that any target has been met.

## Encoded recovery fixtures and extended run

All of the following used the unchanged 40 ms handoff configuration at 640x360,
60 fps and 48 kHz. Encoded gates require absolute per-track median <=16.667 ms
and every marker <=33.333 ms. The separate absolute raw-mixer gate is 2 ms.

| Fixture | Encoded result | Raw result |
| --- | --- | --- |
| Three fresh baseline runs | All six markers per track; median offsets -1.33, -1.67 and -16.67 ms | Both tracks pass in all three runs |
| Producer replacement, OBS sources kept alive | Six markers per track; median -2.33 ms | Both pass; new generation remains on the original sample grid |
| 400 ms producer pause | Six markers per track; median -15.00 ms | Both pass |
| Staggered source creation | Six markers per track; median -14.17 ms | Both pass |
| Synthetic video hide/show | Six markers per track; median -8.33 ms | Both pass |
| One-second mic mute/unmute | Exact intended mic event suppressed; other offsets -7.67 to -7.00 ms | Desktop passes; muted mic uses its explicit five-event assertion |
| Immediate mic mute/unmute | Same intended event suppressed; other offsets -9.67 to -8.33 ms | Desktop passes; muted mic uses its explicit five-event assertion |
| 119-second, six-cycle recording | All 36 markers per track; median -16.33 ms; range -16.67 to -16.00 ms | Both pass; range -0.464 to +0.203 ms |

Offset convention for the encoded table is audio minus video, so negative values
mean audio is earlier. Independent 60 fps video presentation has frame-phase
quantization; these results do not promise identical sub-frame offsets on every
start. The six-cycle run changed first-to-last encoded offset by about -0.33 ms,
within detector precision. It is **not** a 30-minute or physical-clock drift test.

During the six-cycle producer's active interval, status samples showed no
starvation fills, trims, skipped late audio or hard discontinuities. Silence
after the finite producer ends is deliberate and counted separately in the
worker totals; aggregate fill counts alone must not be called dropouts.

The controlled replacement test observed two accepted generations, no hard
discontinuity, and retained a fractional sample-grid phase without an OBS
restart. Offline mappings no longer count as repeated successful reconnects.
The pause fixture is not continuous-music validation, and hide/show only tests
the synthetic video scene item. Mute fixtures have no RNNoise/expander/compressor
chain and do not prove that already submitted samples can be retracted.

The runner now requires both encoded and absolute raw checks to pass (except
the intentionally muted mic's separate fixture assertion). This prevents a
matching relative A/V delay from hiding a wrongly shifted absolute audio clock.
After this combined check was integrated, another three fresh baseline runs
passed both checks on both tracks, using the final offline-reconnect behavior.

## Pending validation

A separate [restart-spanning recording](restart-recording-validation.md) now
checks the entire predecessor/replacement sequence, including rejection of an
explicitly queued stale marker. The earlier warmup replacement fixture above
did not record the interruption itself; its narrower result remains unchanged.

Thirty-minute representative load, physical clocks/capture, source activation
changes, severe handoff stalls, both-machine restarts and real-filter microphone
privacy remain independent proof gates. The 40 ms handoff does not change the intended presentation timestamps
or add another multi-second buffer, but any already submitted audio makes
privacy behavior a real integration question rather than a unit-test guarantee.
