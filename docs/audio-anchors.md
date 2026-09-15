<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Original audio capture anchors

Status: **portable metadata component and synthetic tests only**. This does not
alter the sender, select a network metadata format, prove that the current
nominal converter preserves these anchors, or synchronize physical media.

`include/avsync/audio_anchors.hpp` implements the observability gate described in
[asrc-design.md](asrc-design.md). One `AudioAnchorTracker` belongs to one original
audio device and one explicit conversion segment. A microphone would require a
separate tracker and privacy-generation handling; it cannot inherit a desktop
sample-clock estimate.

## Inputs and exact association

`AudioCaptureAnchor` carries the session/generation, consecutive metadata
sequence, extended original device-frame position, original mapped first-sample
capture time, nominal source sample rate, and explicit discontinuity flag.
Capture and current shared-clock time must both be nonnegative signed 64-bit
nanoseconds. Device/metadata positions are unsigned 64-bit and must not wrap
within a generation. Silence still advances positions and capture time.

The caller supplies the segment's original device-frame origin and corresponding
extended 48 kHz wire-frame origin in `AudioAnchorConfig`. For device position
`p`, the immutable nominal association is:

```
wire_origin + (p - device_origin) * 48000 / nominal_source_rate
```

`ExactWirePosition` retains the whole frame and remainder, with the source rate
as denominator. It is not rounded for each packet, and the denominator need not
be reduced. For example, 147 original frames at 44.1 kHz correspond to exactly
160 wire frames, not the 147 obtained by separately flooring 147 one-frame
blocks. Variable packet sizes do not affect the mapping.

The checked implementation splits quotient and remainder before multiplication.
It supports extended positions beyond `INT64_MAX` without compiler-specific
128-bit integers or floating-point index arithmetic, and rejects an
unrepresentable result rather than wrapping it.

This association is a **nominal conversion contract**, not a claim about which
input samples contributed to an actual resampler output. A sender integration
still must prove its segment origin, fractional phase, and insert/drop/reset
behavior, preserve original anchors alongside converted PCM, and associate them
with the correct stream/SSRC and shared-clock calibration generation. None of
those fields may be reconstructed from packet arrival time. The current header
does not serialize a wire protocol or validate clock calibration itself.

## Health and lifecycle

`observe(anchor, shared_now)` checks original metadata before passing the
original device position and capture timestamp to `SampleRateEstimator`.
`shared_now` only checks freshness; it never enters the rate calculation or
changes capture time. The default age limit is 250 ms, future tolerance 1 ms,
and estimator windows/gap limits are inherited from `RateEstimatorConfig`.

- `priming`, `waiting`, and `measured` return an exact accepted wire position.
  A measured observation retains the estimator's raw and bounded diagnostics.
- Wrong-generation input is rejected without modifying the active stream.
- Invalid metadata, changed rate, explicit discontinuity, nonforward count/time,
  duplicate/reordered/missing metadata sequence, excessive observation gap,
  stale/future capture time, and arithmetic overflow latch a fault. No automatic
  arrival-time or new-anchor rebasing occurs after a fault.
- A fault rejects subsequent same-generation input with `requires_reset`.
  `reset(next, config)` requires a valid new generation/session and explicitly
  supplied conversion origins. An invalid configuration or reused generation
  leaves the old state intact. Session IDs must be unique as required by the
  existing session-token contract; this component stores no session history.

`fresh(shared_now)` checks expiry even when no new anchors arrive.
`current_estimate(shared_now)` only returns a fresh, nonfaulted, in-range rate
estimate. A clamped out-of-range slope remains visible in the measured
observation but is not exposed as an approved correction command. These health
queries are read-only; the controller must stop and explicitly re-prime a new
generation when freshness expires or a slope exceeds its correction limit.
They are not an automatic reacquisition policy. `latest()` and
`latest_wire_position()` are diagnostic snapshots, **not freshness guarantees**.

Storage is constant: one accepted anchor, its exact association, the estimator's
points, one latest rate estimate, and saturating diagnostic counters. There is no
PCM queue, unbounded anchor history, allocation in `observe`, timestamp rewrite,
resampler, or rate servo. Sequence gaps deliberately fail closed in this first
gate; a future recovery policy would need independent evidence of continuous
conversion and source positions before relaxing that rule.

## Synthetic coverage

`avsync-audio-anchor-tests` independently generates original device positions and
continuous shared-clock times for 31 simulated seconds per case at 44.1, 48, and
96 kHz, at 0, +/-100, and +/-500 ppm. It varies chunk sizes from 1 to 1,920 frames,
adds arrival jitter without changing original timestamps, and starts the device
counter above `2^63`. The recovered raw rate must be within 0.002 ppm of the
independently generated rate; arrival jitter must not change the estimate.

Those observability fixtures use a 501 ppm estimator limit solely to avoid
misclassifying nanosecond-quantized exact-boundary +/-500 ppm timestamps. They
do not change the production default 500 ppm limit or claim that threshold
quantization is resolved in a live controller. A separate out-of-range fixture
verifies that a 1,000 ppm slope remains diagnostic and cannot become an approved
command.

The same positions paired with manufactured nominal 48 kHz timestamps estimate
approximately zero ppm in every drift case. This is a reproducible demonstration
that nominal RTP progression alone can hide original oscillator drift, not a
measurement of an actual device's drift. Exact rational conversion, large
counters, overflow, original-anchor preservation, negative timestamps, missing
metadata, invalid rates, clock-age bounds, and explicit resets have additional
fixtures. No test in this target captures audio, changes services, or establishes
physical A/V synchronization.
