<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Timing core: implemented contracts and limits

This dependency-free C++20 library is a **synthetic timing foundation**, not a
working capture bridge or an end-to-end synchronization result. It does not
capture devices, synchronize host clocks, send media, resample audio, or drive
OBS. Passing its unit tests establishes none of those capabilities.

## Shared time and generations

All `Nanoseconds` values are signed 64-bit values in one externally established
session time domain. Negative values are valid. A `SessionToken` contains a
nonzero session ID and generation. The controller must issue unique session IDs;
within one session a reset must increase the generation. The library cannot
detect reuse of an ID from a historical session it no longer knows.

`CaptureToPresentation` applies this exact mapping:

```
presentation = capture + total_delay + path_offset
```

Construction checks that the combined offset is representable and nonnegative.
Mapping rejects a mismatched token or an overflowing result. The path offset is
a separately calibrated content-path correction; the library does not discover
it from arrival times. The proposed initial total synchronizer target is two
seconds, not two seconds added to a preceding transport buffer. Integrators
must budget capture, packet recovery, processing, and scheduling within that
target and document unavoidable latency outside their timestamp reference.

Capture timestamps and presentation timestamps remain distinct. Never rebase
each process to its own first packet or silently use arrival time when capture
time is missing. A capture reset, clock step, or invalid timing mapping should
trigger a controller-issued generation change. Reordered network packets alone
are not a clock discontinuity.

## Bounded, timestamp-scheduled metadata queue

`BoundedTimelineQueue` stores metadata, not media payloads, ordered by
presentation time and then sequence. It rejects wrong generations, duplicate
queued sequence IDs, invalid duration/size, arithmetic overflow, already-late
items, and items whose end exceeds the future horizon. Independent count,
aggregate byte, and earliest-start-to-latest-end span bounds are enforced.
All limits must be positive. All failures leave existing entries unchanged.

The byte field is an accounting contract: the caller must supply the actual
retained payload size. This queue cannot inspect or constrain memory held
elsewhere. A caller retaining large image buffers must also bound its buffer
pool and release rejected/expired payloads. `pop_due` returns expired metadata
identities for this purpose. A successful reset clears all metadata; the owner
must clear the corresponding payloads as the same operation.

`pop_due(now, allowed_lateness)` never returns a future item. It discards older
expired entries and returns at most one due item within the specified tolerance.
Zero tolerance means exact-deadline matching and is primarily useful in tests;
real schedulers must choose an explicit, bounded tolerance. The queue does not
choose audio concealment or video repeat/drop policy. Silence represented by
valid PCM blocks is ordinary media, not a missing packet.

No capacity or delay is inferred from a startup frame count. A half-second gap
in a nominal 60 fps input therefore does not change later capture-to-presentation
mapping. The queue is a bounded ordinary-thread component, **not thread-safe or
real-time allocation-free**. Synchronize access externally; never block an OBS
callback on this queue, network input, or IPC.

## Exact sample and frame progression

`RationalTimeline(origin, numerator, denominator)` returns
`origin + floor(index * 1e9 * denominator / numerator)`. Both rate components
must be nonzero. For example, 60000/1001 fps and 48000/1 samples per second are
represented without accumulating error from repeatedly rounded increments.

Portable quotient/remainder arithmetic avoids intermediate multiplication
overflow without relying on compiler-specific 128-bit integers. A result
outside signed nanosecond range returns no value. Index zero returns the origin.

## Sample-clock estimation is not resampling

Create a separate `SampleRateEstimator` for each independent audio device.
Observations pair a device sample position with the capture timestamp of that
position in the shared clock. Do not feed network arrival timestamps or sample
counts after concealment/resampling into this estimator.

It measures the counter slope over nonoverlapping windows, with constant-size
state. The first observation primes it. Nonincreasing sample/time values,
unrepresentable time differences, or an excessive observation gap re-prime it
and report a discontinuity. Wrong-generation observations do not alter state.
The control plane decides whether a reported discontinuity requires a new
generation and queue flush.

Positive `source_rate_error_ppm` means the input device runs faster than the
shared clock. The recommendation clamps that error to the configured bound:

```
output_samples_per_input_sample = 1 / (1 + bounded_ppm / 1e6)
```

An out-of-limit estimate sets `within_correction_limit` false; the clamped value
is **not permission to continue playback indefinitely**. An integrator should
enter its controlled re-lock/fault policy. This library has no audio converter,
ratio ramping, PLL, anti-jitter feedback, or quality guarantee. A real ASRC must
apply smooth, independently validated correction. One receiver-side owner per
audio stream should control that correction; competing rate controllers need
explicit analysis.

## Microphone privacy

`MicPrivacyGate` starts and resets muted. Check `permits` at actual delivery, not
only when a future block enters a queue. A mute immediately makes all delivery
checks fail. An unmute establishes a new capture-time cutoff: all earlier and
straddling blocks are rejected, including voice buffered before the mute and
voice captured during it. This conservative whole-block policy may discard a
small amount of new speech at the boundary.

Commands older than the last accepted control time are rejected. Repeating the
current mute state is idempotent and does not advance the audio cutoff, so a
health check cannot repeatedly discard buffered speech. The gate does not provide
thread synchronization; order control commands and output delivery explicitly.
Do not submit multiple seconds of future microphone audio to an output API that
cannot retract it. Microphone failure must remain independent of desktop/video
operation in the eventual transport controller.

## Verification boundary

The tests use explicit checks that execute with `NDEBUG`/Release enabled. They
cover safe arithmetic, exact rational progression, finite queue limits,
reordering, a startup frame gap, late media, stale generations, reset behavior,
100/500 ppm slopes, exceeded correction limits, and mute/unmute cutoffs.

Remaining integration proof gates include capture timestamp accuracy, shared
clock acquisition, preservation through transport, deadline-bounded packet
recovery, real ASRC quality, device restart behavior, resource use, and actual
recorded A/V timing. A synthetic passing result must not be described as
hardware, live audio, or OBS validation.
