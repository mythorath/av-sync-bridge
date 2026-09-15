# Offline audio-rate components — 2026-09-15

This checkpoint implements a **bounded DSP backend, an original capture-anchor
metadata policy, and generated-media fixtures**. It does not enable a live rate
controller, change production audio, or establish physical A/V sync. Normal OBS,
its sources/mutes, audio senders/receivers, capture devices and startup tasks were
left running unchanged. Only matching libsamplerate development files were added
on the test Linux host; its existing runtime was not upgraded.

## What exists now

- [AsrcStereo](asrc-backend.md): explicit stereo F32, bounded input/output spans,
  stateful best-quality sinc conversion, finite +/-500 ppm commands, exact
  consumed/generated counts, and reset/EOS/failure handling.
- [AudioAnchorTracker](audio-anchors.md): original device-position/shared-clock
  observations, exact rational nominal conversion positions, generation/freshness
  validation and a rate estimate independent of packet arrival/nominal RTP time.
- `avsync-asrc-fixture`: analytic tones, unique Gaussian pulse markers, silence
  and DC, all generated and compared in memory. No PCM files, playback or devices.
- [Current conversion probe](conversion-timing.md): a separately generated test
  that **demonstrates the existing sender conversion's observability failure**.

These are separate components. An exact rational *ideal* conversion association
does not prove that actual converted samples retained that association, and a
resampler accepting ratio commands is not a completed adaptive clock controller.

## Independent signal/count method

The generated input samples a continuous-time signal at
`48000 * (1 + source_ppm / 1000000)` Hz. The output is compared with the same
analytic signal at the independent fixed 48 kHz grid. The correct output/input
ratio is inverse to the source error. A known-rate test supplies that ratio;
`--estimate` instead uses at least three measurements from a generated original
192 kHz device-position/shared-clock preflight. That preflight is not a live
priming queue, a timestamp transport test or a changing-rate controller.

Six marker centers have fractions 0.12, 0.22, 0.365, 0.49, 0.665 and 0.84 of the
requested duration. Their unequal spacing and complete-region count prevent
selecting a convenient repeating cycle. Pulses have a 1 ms Gaussian sigma;
peak positions are compared to the predefined centers, without offset fitting.

Fixed calls use 480-frame input/output capacity. Varied calls use predetermined
1..1920-frame inputs and 1..480-frame outputs, retaining every unconsumed suffix.
All PCM storage is fixed-size; metrics use a fixed histogram rather than keeping
an unbounded list. Each run allows 4..600 simulated seconds, at most five million
calls and a 120-second wall budget; the runner enforces a 130-second child timeout.
No wait matches the simulated media duration.

The signal gates are:

- Exact consumed input and final output count within one endpoint-rounding frame
  of **both** the command-based ratio and the independent source-clock duration.
- First/last 0.25 seconds excluded from residual/gain comparison; guarded RMS
  residual <= -80 dBFS, maximum residual <= -50 dBFS, gain within 0.2 dB.
- Silent right-channel peak <= -100 dBFS, whole-clip left peak <= full scale,
  silence <= 1e-7, all output finite, and all six marker peaks within one sample.

Whole-clip peaks remain reported even outside the guard. The converter does not
clip or normalize them. An initial arbitrary 0.52 peak gate rejected an 18 kHz
case because finite-segment boundary ringing reached approximately 0.535. The
gate was corrected to full scale with the 0.5 source amplitude; guarded residual
and gain gates were retained. This is **not** a claim of zero transient overshoot.

A second initial test weakness was found with stationary DC: the old count check
followed the commanded ratio, so a wrong unity command on +500 ppm input passed
with 192,096 frames instead of the intended 192,000 in a four-second case.
The independent source-duration gate now rejects it, including silent inputs
whose waveform alone cannot expose drift. The Python runner independently
recomputes that count with integer arithmetic and verifies the positive quality
gates; it does not merely trust the executable's `passed` boolean.

## Measured generated-signal results

Linux GCC 13.3 RelWithDebInfo, libsamplerate 0.2.2:

| Matrix subset | Cases | Observed result |
| --- | ---: | --- |
| 0, +/-100, +/-500 ppm; 1/10/18 kHz tones, markers, silence, DC; fixed/varied chunks | 60 | All passed |
| Original-anchor preflight at 0, +/-100, +/-499 ppm; markers and 1 kHz tone | 10 | All passed |
| Deliberately wrong unity command at +/-500 ppm for all four signal kinds | 8 | All correctly failed the independent timing gate |
| 600-second simulated +500 ppm markers, fixed chunks | 1 | Passed |

The full suite's **71 positive and eight negative cases** behaved as expected.
Across its positive cases, worst guarded RMS residual was -108.733 dBFS, maximum
guarded sample residual -101.092 dBFS, maximum whole-clip peak 0.535357, and source
duration error at most one output frame. These numbers describe this specified
matrix, not arbitrary material or perceptual listening quality. A separate
estimated +100 ppm/18 kHz eight-second case also passed, with guarded RMS
residual -94.981 dBFS; estimator timestamp quantization matters at high frequency.

The 600-second generated marker run consumed 28,814,400 input frames and produced
exactly 28,800,000 output frames. It found all six markers within the one-sample
gate, with zero final duration error. It completed in 34.596 wall seconds.
This is a **constant-rate, known-ratio accelerated fixture**, not ten minutes of
live audio or a demonstration of a changing-rate controller.

Observed per-call p99 upper histogram bins reached 3.46 ms across the positive
matrix, and the largest measured call was 5.542 ms. The long marker case's p99
bin was 1.30 ms and maximum 5.127 ms. Calls were timed around the backend plus
its validation, excluding signal generation/reference analysis. Fixed 480-frame
calls are the comparable nominal 10 ms case; varied call percentiles are not
10 ms dispatch percentiles. Background workloads were not controlled.
**The proposed universal p99-under-2-ms performance gate is not satisfied.**
No allocation trace, unique-memory measurement, hard real-time guarantee or
representative thirty-minute load result is claimed.

## Conversion observability: a measured open gate

The generated current-converter test reproduced the same fixed-chunk result on
Windows GStreamer 1.28.6/MSVC 19.44 and Linux GStreamer 1.24.2/GCC 13.3:

| Original rate error | Nominal source duration | Output sample count | Observed PTS jump |
| --- | ---: | ---: | ---: |
| +500 ppm | 90 seconds | 4,320,000 | -31.254373 ms |
| -500 ppm | 90 seconds | 4,320,000 | +31.255627 ms |

The output slope within segments was approximately **zero ppm**, although the
independent original anchors recovered the injected mismatch. One phase step
occurred at approximately 62.5 nominal seconds. Sample counts alone therefore
do not prove timing continuity. The Windows varied-buffer controls additionally
show why DISCONT flags must be counted separately from actual phase steps.
See [complete method and limits](conversion-timing.md), including the fact that
generated zero PCM cannot prove audible artifacts or individual sample identity.
These are not measurements of the user's actual device drift or the root cause
of any particular production incident.

## Regression coverage and remaining work

The bounded backend passed 431,962 unit checks in optimized and ASan/UBSan builds.
Original-anchor metadata passed 202,680 checks, including independent clock rates,
exact nonintegral nominal mapping, large counters, malformed/reordered/stale
anchors, wrong generations, reset, and arrival jitter that cannot alter the rate.
Windows default Release passed six CTest entries. Linux optional-ASRC/network
Debug with ASan/UBSan passed twelve entries, including the five-case quick
generated suite. The Python suite passed 71 tests. The system DSP library itself
was not rebuilt with sanitizers; these checks are not exhaustive race testing.
An instrumented Linux conversion run also completed the 90-second varied +500 ppm
fixture without reported address/leak/undefined-behavior errors: 4,320,001 output
frames and one -31.269241 ms PTS step, consistent with the measured open timing
gate. Running safely does not turn that conversion behavior into a sync pass.

Still required before live activation:

1. A nominal sender converter with verified sample/count/phase continuity and
   original-device timing preserved independently of its nominal output PTS.
2. A versioned, validated anchor-to-PCM transport association, including SSRC,
   restart/calibration epochs and loss/reordering behavior.
3. One output-time-based, slew-limited controller with independent changing-rate,
   partial-call, phase-error, freshness, resource and reset fixtures.
4. Combined real audio/video IPC/OBS playout, physical unique-event calibration,
   sustained load and the full restart/microphone privacy matrix.

The current component stays opt-in and offline. See [build/run instructions](building.md)
and the [complete-system design](asrc-design.md). Do not replace working production
audio with this checkpoint.
