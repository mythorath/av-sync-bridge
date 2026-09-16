<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Phase-guard validation — 2026-09-15

The [model-based guard](audio-phase-guard.md) is implemented inside the offline
desktop correction worker. **Its generated-media safety/consistency tests pass;
live activation remains off.** No capture device, service, production OBS source,
microphone mute or startup route was changed by these tests.

Runtime: Linux x86-64, GCC 13.3, RelWithDebInfo, libsamplerate 0.2.2, AMD Ryzen 7
1700 with eight online cores. Separate Debug address/undefined-behavior-sanitized
tests and Windows MSVC portable tests also ran. The packaged resampler library
itself was not sanitizer-instrumented.

## Independent PCM evidence

Four 600-second generated linear-waveform runs compare predicted fractional
source position with position decoded from the real resampler's PCM output.
The input ramps and waveform decoder do not use the model's ratio recurrence.
Startup/wrap guards deliberately exclude filter-boundary transients; these
results do not establish boundary quality or whole-cycle correctness.

| Ratio-command case | Maximum observed source-position discrepancy |
|---|---:|
| Unity | 0.000982128 frames |
| +499 ppm | 0.004088201 frames |
| -499 ppm | 0.003452741 frames |
| Repeated bounded ramps between +/-499 ppm | 0.005506821 frames |

All four pass the explicit **0.05-frame** gate. The largest measured discrepancy
is about **0.115 microseconds at 48 kHz**, across 211,620,917 checked channel
samples. This is model-versus-generated-waveform agreement, **not physical clock
or A/V accuracy**. Maximum modeled endpoint-command change was 0.99 ppm per
480-frame call; no public API directly measured the library's reached ratio.

A separate 12-second wrong-model control drives real +499 ppm DSP while the
model remains at unity. It measures **287.424539134 frames** of disagreement and
is rejected. The final short waveform suite includes this negative control.

## Guard behavior and no-false-alarm matrix

- The repeated-transition `cycle` fixture naturally accumulates feed-forward
  error. At approximately **160.12 simulated seconds**, predicted disagreement
  reaches **10.005164 ms**. The worker faults before processing/releasing that
  quantum, clears both PCM queues, and refuses old-generation delivery/dispatch.
  This is an expected safety shutdown, not successful continuous synchronization
  under indefinite oscillator changes.
- Units inject an **11 ms original-timestamp shift**, which also triggers the
  phase fault. Reset requires an explicit successor and clears phase history.
- Withholding bracketing anchors produces no DSP calls or command advancement;
  the worker does not invent future timestamps. The existing stall deadline
  prevents indefinite waiting.
- All **14 ten-minute nonuniform-marker cases** pass with the guard active and
  no false faults. They cover two packet/read partitions at 0, +/-100, +/-499 ppm,
  one step sequence and a slow ramp. Each finds all 12 events with no extras.
  Maximum marker error remains **3.458334 ms**; paired-partition difference remains
  **0.020231 ms** (less than one output sample).
- Maximum guard-predicted disagreement in clean step/ramp cases was respectively
  **3.456703 ms / 0.585569 ms**. Guard and marker maxima occur at different samples
  and have different measurement resolutions; they are not identical metrics.

The coherent wrong-anchor control is intentionally retained: -499 ppm generated
PCM falsely labeled with +499 ppm timestamps produces **55.468852 ms** marker
error, while the guard reports only **2 ns** internal disagreement. The external
oracle rejects it; the guard cannot. This explicitly tests and documents the
trust boundary rather than claiming metadata can verify itself.

## Bounds and regressions

- **481,554 portable model/ledger checks**, including large absolute source
  positions, fractional anchors, invalid/overflowing times and counters,
  interpolation coverage, capacity/pruning, version rejection and reset.
- **93,494 correction-worker checks**, extending the prior queue/epoch/stall
  suite with bracketing-anchor waits, timestamp-shift faults and phase reset.
- Linux optimized and sanitized builds: **26 CTests** passed. The updated
  wrong-model waveform control was also rerun under sanitizers.
- Windows: **8 portable** and **15 network** CTests passed. Python helper tests:
  **76 on each host**. No Windows resampler binary is claimed.
- Worker object: **256,144 bytes** in the tested ABI, excluding backend storage.
  Clean runs retained at most **6 of 512** ledger anchors. PCM ring peaks remained
  **2,227 input / 2,400 output** frames. Per-generation model horizon is 24 output
  hours; this limit is enforced, not evidence of a completed 24-hour soak.

Loaded accelerated runs overlapped other finite tests. Whole multi-quantum
dispatches reached **13.17 ms p99** in one case and **73.53 ms maximum** across
the matrix. These are not individual-call timings or paced real-time scheduling
passes. The original 2 ms/10 ms-quantum resource gate remains unproven.

## Next gates

1. Use the independently checked timing model to evaluate tone/multitone gain,
   residuals, local discontinuities and startup/reset behavior across the full
   changing-ratio quality matrix.
2. Trace allocations and memory growth; measure individual-call cost and paced
   deadline behavior under representative load. Optimize only from those results.
3. Validate capture-anchor uncertainty and the combined nominal-converter/RTP/
   worker/presentation path before isolated OBS and physical A/V testing.

The guard is a stop-on-disagreement safeguard, not a phase-catching servo,
automatic reconnection service, calibration substitute or microphone privacy
guarantee. Production routing remains unchanged.
