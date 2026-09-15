<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Current sender conversion: offline timing observability

`avsync-conversion-timing-probe` is an **offline generated-media diagnostic** for
[ASRC gate zero](asrc-design.md#gate-zero-prove-that-drift-remains-observable).
It does not capture sound, use network transport, play audio, write PCM files,
change an endpoint, or interact with OBS. It is not an ASRC implementation or a
measurement of any physical audio oscillator.

## Run

Build with `AVSYNC_BUILD_NETWORK=ON`; the executable uses the same optional
GStreamer dependency as the experimental sender. Help/no arguments do nothing
beyond printing usage. An explicit run is required:

```sh
avsync-conversion-timing-probe --offline --seconds 90 --ppm 500 --chunk-pattern varied
avsync-conversion-timing-probe --offline --seconds 90 --ppm -500 --chunk-pattern fixed
avsync-conversion-timing-probe --offline --seconds 10 --ppm 0
```

`--seconds` is **nominal source-frame duration**, 1..300; this is accelerated
processing, not a wall-clock wait. The entire run has a 90-second wall deadline.
`--ppm` is -500..500, with positive meaning a faster original device oscillator.
The fixed chunk is 1,920 input frames. The varied pattern repeats
1, 383, 1,919, 1,921, 960, 4, 3,840, 127 frames, including chunks that do not divide
the 4:1 conversion ratio. The final chunk is trimmed exactly.

Exit 0 means a valid fixture completed and either the zero-drift control ran or
the nonzero hidden-drift observation was obtained. **It is not a gate-zero pass.**
Exit 1 is an execution error, 2 invalid arguments, and 3 an invalid/inconclusive
observation. Read `fixture_pass`, `gate_zero_pass`, and `observation` separately.
Gate zero remains false because this executable tests the current output-PTS-only
path, which carries no independent original-anchor transport. A zero-error input
cannot demonstrate whether nonzero error remains observable.

## Independent oracle and matching conversion

Each input buffer contains generated eight-channel F32 zero samples, without the
GAP flag. No signal-quality or individual-sample identity claim follows from
zeros. Original frame positions and shared-clock capture times are generated
independently of conversion output. For relative original frame index `n`:

`capture(n) = 10,000,000,123 ns + floor(n * 10^15 / (192000 * (1000000 + ppm)))`

The timestamp origin is intentionally not on a sample boundary. Buffer offsets
use an unrelated nonzero device-frame origin. Input durations remain nominal
frame durations, matching the existing sender, while the first-sample PTS follows
the independently drifting capture clock. Only the first input has DISCONT.
There are no injected gaps, packet drops, timestamp steps, or subsequent flags.

The conversion is the sender's existing explicit eight-channel-to-stereo matrix
and shared 0.9 row normalization, 192 kHz F32 stereo, quality-10 Kaiser
`audioresample` with automatic sinc table selection, then undithered/unshaped
48 kHz stereo S24BE. No output timestamp is rewritten. The sink is a metadata-only
`fakesink`, with scheduling and last-sample retention disabled. RTP packetization,
RTCP and the network clock are deliberately absent so conversion is isolated.

The appsrc byte reservoir is limited to 614,400 bytes (100 ms of the generated
input), with at most 32 buffers. A single producer checks both current levels
before each push and yields when full; no buffer is intentionally leaked or
dropped. One buffer may also be processing in the downstream chain. This differs
from the live sender's overflow policy: the fixture paces an accelerated generator
instead of modeling live overflow. The whole-process memory footprint includes
GStreamer and its resampler, not just that reservoir. Queue levels describe the
source reservoir, not a total memory measurement.
[appsrc queue API](https://gstreamer.freedesktop.org/documentation/app/appsrc.html)

## Reading the results

- `original_anchor_ppm` fits original frame progression against independently
  generated shared-clock time, before conversion.
- `within_output_segment_ppm` fits accumulated output-frame progression against
  output PTS, excluding observed phase steps greater than 1 ns. Near zero for a nonzero
  original slope demonstrates that output PTS alone hides that slope.
- `max_within_segment_nominal_error_ns` compares each output PTS with its segment's
  first PTS plus the nominal 48 kHz sample count. Rounding can differ by 1 ns.
- `later_discontinuities` counts output DISCONT flags after the initial output.
  They are **not equivalent to observed timeline resets**: very small input
  chunks can lead to a later flag without a PTS step even at zero drift.
- `output_phase_steps` separately counts output PTS deviations greater than 1 ns
  from the previous buffer's end. The first 16 events of each kind report output
  index, PTS and the signed step. Every event is counted after either list fills.
- `output_count_difference` compares actual EOS-drained frames with the exact
  nominal 4:1 count. A reset can affect sample history/count as well as PTS.
- `max_ideal_anchor_error_before_phase_step_ns` uses the *ideal* fixed-ratio association
  `output_index * 4` to original frames. It is not measured sample provenance;
  the comparison stops after the first observed phase step. Actual individual
  sample identity is explicitly unproven, including across zero-step flags.

The audited resampler builds a nominal sample-count timeline within each segment
and recognizes sufficiently large input timestamp/sample-count disagreement as a
discontinuity. This predicts a nominal slope followed by occasional phase steps,
not continuous preservation of the original slope. The diagnostic records what
the installed runtime actually does instead of assuming that source inference
is a measurement.
[Pinned resampler source](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c#L660-L981)

## Measured Windows offline observations

On 2026-09-15 the isolated Windows GStreamer **1.28.6** SDK and MSVC **19.44**
Release build produced the following generated-media observations. The four
90-second runs each processed 17,280,000 original frames in about 13 wall seconds.
Every run completed and reported `gate_zero_pass=false`; no endpoint or transport
was involved.

| Nominal source duration | Original ppm | Chunks | Output frames | Observed phase steps | Signed phase step |
|---|---:|---|---:|---:|---:|
| 2 s | 0 | fixed | 96,000 | 0 | none |
| 2 s | 0 | varied | 96,000 | 0 | none |
| 90 s | +500 | fixed | 4,320,000 | 1 | -31,254,373 ns |
| 90 s | -500 | fixed | 4,320,000 | 1 | +31,255,627 ns |
| 90 s | +500 | varied | 4,320,001 | 1 | -31,269,241 ns |
| 90 s | -500 | varied | 4,320,001 | 1 | +31,237,900 ns |

The independently recovered nonzero input slopes were within 0.00001 ppm of the
injected +/-500 ppm, while within-segment output slopes were within 0.00001 ppm of
**zero**. Phase steps occurred after approximately 62.5 nominal source seconds;
the ideal pre-step anchor discrepancy grew to approximately 31.25 ms. The varied
conversion drained one extra frame relative to the ideal nominal ratio.

The varied zero-drift control produced 31 later DISCONT flags despite zero phase
steps and an exact frame count. The varied +500/-500 runs produced 1,417/1,416
flags respectively, but only **one observed phase step each**. This is why the
report counts flags separately instead of calling every flag a timing reset.
Exact audible discontinuities and sample identity remain unmeasured by zero PCM.

## Scope of the next fix

Keep live correction disabled until immutable original-device/shared-clock
anchors and their exact conversion-segment association survive to the receiver.
The sender's reset behavior must be addressed explicitly; a receiver resampler
cannot recover metadata that the sender discarded. A raw nominal converter with
a separate sample ledger is a possible next step, not something this fixture
silently enables. Audio quality, exact marker identity, clock transport,
restarts, sustained load, microphone privacy, and physical A/V alignment remain
separate tests.
