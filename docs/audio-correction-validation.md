<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Offline correction-worker validation — 2026-09-15

Historical checkpoint `7c76172`. The later [phase-guard report](audio-phase-validation.md)
adds runtime consistency checks; the measurements and open gates below describe
the original worker checkpoint, not a claim that the later monitor is absent.

## Scope and verdict

The [bounded desktop worker](audio-correction.md) now combines original-record
validation, original-clock rate estimation, stable acquisition, owned PCM,
fixed-quantum feed-forward correction and a timestamped output ring.
**The measured offline milestone passes; live activation is still blocked.**
This is not microphone support, live network correction, physical A/V sync,
production recovery or an OBS timing result.

The final local runtime was Linux x86-64, GCC 13.3, RelWithDebInfo and
libsamplerate 0.2.2, on an AMD Ryzen 7 1700 with eight online cores. No capture
device, normal OBS source, startup service or mute control was changed by the
fixtures. Tests generate PCM in memory and print aggregate diagnostics only.

## Independent long matrix

The analytic source clock is independent of worker commands/output. Twelve
nonuniform pulses are sampled on that source clock, with original 192 kHz
device-position anchors and a known nominal 48 kHz association. The fixture
supplies nominal F32 PCM directly; the separately tested sender converter and
RTP transport are **not** in this matrix. The marker oracle detects every event
above threshold before comparing the complete ordered event list to truth; it
does not pick a convenient repeating cycle or subset.

Every positive case has 600 simulated seconds, including acquisition, and two
packet/read partitions. Events in the step/ramp fixtures include the transitions
themselves as well as the later tail. A deterministic 0..4 ms arrival variation
does not change the underlying clock model.

| Clock case | Maximum absolute marker error, either partition |
|---|---:|
| 0 ppm | <0.000001 ms |
| +100 ppm | 0.009887 ms |
| -100 ppm | 0.007726 ms |
| +499 ppm | 0.010071 ms |
| -499 ppm | 0.010171 ms |
| 0 -> +499 -> -499 -> 0 ppm | 3.458334 ms |
| -300 -> +300 ppm ramp | 0.590926 ms |

All **14 positive cases** passed the 5 ms marker and 5 ppm final-command gates,
with all 12 markers, finite output and zero measured right-channel leakage.
Constant-rate errors are less than one 48 kHz sample (0.020833 ms).
Maximum paired-partition difference was **0.020231 ms**, also below one sample.
The output-read-only unit comparison was bit-identical.

The deliberate **60-second wrong-anchor control** produced **55.468852 ms**
maximum error and was correctly rejected by the phase oracle. Its metadata
remained syntactically valid: this demonstrates why metadata health and a rate
command alone do not prove physical synchronization. This control validates the
test oracle, **not a runtime phase monitor**; that monitor is not implemented.

The worker never resets during clean positive cases. It discards about three
seconds of acquisition PCM, then retains only bounded current data. It does not
EOS-drain at the finite fixture end, so its retained tail is deliberately not
counted as completed output. No exact total-duration/endpoint-count claim is
made for this live-style stop.

## Safety and regression checks

- **88,537 worker checks:** full/partial output reads, packet partitions, priming,
  bad PCM, wrong/reused generation, missing input, out-of-range rate, unhealthy
  upstream, backward caller time, queue/full-output backpressure, stale data,
  caller-buffer reuse, pending-media reset and reset to exact silence.
- A dedicated stall test empties both public queues while the backend still
  has private history. A 201 ms no-progress interval must fault; that history
  cannot be replayed just because public queues are empty.
- Linux optional network/ASRC build: **23 CTests** in optimized and address/
  undefined-behavior-sanitized configurations. The short controller matrix is
  included in both; library binaries themselves are not sanitizer-instrumented.
- Windows MSVC: **7 portable** and **14 network** CTests; **76 Python helper
  tests** on Windows and Linux. These do not claim a Windows ASRC build.

## Resource observations, not certification

The final worker object occupies **236,088 bytes** in the tested Linux ABI,
excluding the separately allocated backend. Observed ring peaks were **2,227
input frames** and **2,400 output frames**, below the 9,600/3,840 caps. Each
dispatch stayed within eight calls/3,840 output frames. The largest measured
command step was **0.99 ppm per 480 output frames**; zero-progress calls and
insufficient output space did not advance it.

Fixture timings measure whole, potentially multi-quantum dispatches, not the
individual 10 ms DSP-call p99 required by the design. Accelerated matrix runs
used two concurrent workers and sometimes overlapped builds/sanitizer tests.
One preliminary loaded run recorded a **149.6 ms maximum dispatch**. Do not
reinterpret simulated media time or average throughput as real-time scheduling
success. CPU deadline/headroom certification remains open.

## Remaining blockers before live correction

1. **Runtime phase/source-position accounting and hard fault:** the fixed output
   grid and feed-forward rate do not guarantee indefinite lock. Do not infer
   exact source provenance from libsamplerate's input-consumed count or subtract
   a guessed filter delay. Establish a defensible bounded ledger/monitor.
2. **Changing-ratio waveform oracle:** measure reached-ratio slew, tone gain,
   local discontinuity/residual limits and channel behavior across the complete
   quality matrix. Pulses and finite output alone do not establish sound quality.
3. **Resource gates:** allocation tracing, initialization failures, backend
   storage/RSS behavior, individual-call timing and representative paced load.
4. **Combined boundaries:** actual nominal converter -> ordered/decoded RTP ->
   worker -> shared presentation queue -> isolated OBS, then unique physical
   A/V events, restarts and sustained-load measurements.

No production service migration, OBS offset adjustment, physical content-time
calibration, microphone privacy guarantee or automatic live recovery follows
from this report. Keep the existing audio path until those gates pass.
