<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Receiver ASRC: design and staged implementation

Status: **complete-system design with offline-tested components, 2026-09-15**.
The [bounded DSP backend](asrc-backend.md), [original-anchor metadata policy](audio-anchors.md)
and [generated-signal fixture](asrc-validation.md) are now implemented separately.
The [original-anchor wire diagnostic](audio-anchor-transport-validation.md) now
preserves that timing separately from nominal RTP progression. A bounded
[offline feed-forward worker](audio-correction.md) implements acquisition,
fixed-quantum command control and failure/reset handling. A separate
[model-based phase guard](audio-phase-guard.md) now monitors that offline worker;
live integration and full uncertainty/quality/resource gates remain open. There is still no hardware/OBS
synchronization claim. The [old conversion diagnostic](conversion-timing.md)
measured the gate-zero failure predicted below; live activation stays off.
The contracts in [network-clock.md](network-clock.md),
[audio-conversion.md](audio-conversion.md), and [timing.md](timing.md) remain
authoritative. This first milestone concerns desktop audio, not the microphone.

## Decision

Use **libsamplerate's stateful `src_process` API**, initially
`SRC_SINC_BEST_QUALITY`, in an optional Linux receiver-worker component. It adds
one library, not another media framework, daemon, network protocol, or OBS
plugin. Keep the portable timing core dependency-free. Audit the installed
version before building; the source review here pins libsamplerate **0.2.2**,
GStreamer **1.28.6**, and libsoxr **0.1.3**, without claiming those are the newest
available releases.

This is a selection for a finite implementation/measurement milestone, not a
claim that one library sounds better. Do not add automatic alternative backends.
If its timing or CPU gates fail, report that failure before selecting another.

| Candidate | Relevant contract | Decision / limitation |
|---|---|---|
| GStreamer `GstAudioResampler` | `update(in_rate, out_rate, options)` supports changes; `VARIABLE_RATE`/interpolated tables support that use. Frame-count and maximum-input-latency queries exist. | No new dependency, but integer rates, update-phase rounding, and caller-owned ramping need additional proof. At unscaled 48 kHz, a 1 Hz change is about 20.83 ppm. Scaled integer ratios are possible, not validated here. |
| libsamplerate | `SRC_DATA.src_ratio` is a double **output/input** ratio; stateful processing reports consumed/produced frames. Ratio changes are smoothed; `src_set_ratio` deliberately requests a step. | Selected. Use a bounded output-block ramp; do not treat packet size as the desired ramp duration. There is no public filter-delay getter in the audited header, so timestamp/impulse fixtures are mandatory. |
| libsoxr | `SOXR_VR`, `soxr_set_io_ratio` with **input/output** ratio, and an explicit slew length in output frames. Creation must cover the maximum planned I/O ratio. | Credible alternative, but its variable engine's apparent delay API is not trustworthy for this purpose: see the source finding below. |

Primary contracts: [GStreamer API](https://gstreamer.freedesktop.org/documentation/audio/gstaudioresampler.html),
[GStreamer flags](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-resampler.h#L178-L200),
[GStreamer update implementation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-resampler.c#L1377-L1480),
[libsamplerate API](https://libsndfile.github.io/libsamplerate/api_full.html),
[libsamplerate header](https://github.com/libsndfile/libsamplerate/blob/0.2.2/include/samplerate.h),
[libsoxr variable-rate example](https://github.com/chirlu/soxr/blob/0.1.3/examples/5-variable-rate.c#L32-L58).

**Important libsoxr finding:** although `soxr_delay()` is documented in output
samples, the selected variable engine's `vr_delay()` returns the constant 100
with a TODO. Its `vr_create()` also ignores the supplied quality/runtime
structures. This does not prove poor audio quality; it means neither the delay
value nor fixed-rate quality-profile assumptions establish variable-rate timing
or quality. Do not subtract 100 samples from capture PTS.
[Public declaration](https://github.com/chirlu/soxr/blob/0.1.3/src/soxr.h#L149-L175),
[variable engine](https://github.com/chirlu/soxr/blob/0.1.3/src/vr32.c#L572-L591),
[engine selection](https://github.com/chirlu/soxr/blob/0.1.3/src/soxr.c#L389-L424).

The pinned library notices are **BSD-2-Clause** for libsamplerate,
**LGPL-2.0-or-later** for GStreamer's resampler, and **LGPL-2.1-or-later** for
libsoxr. Preserve the actual packaged notices and applicable redistribution
requirements; no library implementation needs copying into this repository.
[libsamplerate COPYING](https://github.com/libsndfile/libsamplerate/blob/0.2.2/COPYING),
[GStreamer notice](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-resampler.h#L1-L17),
[libsoxr LICENCE](https://github.com/chirlu/soxr/blob/0.1.3/LICENCE).

## Gate zero: prove that drift remains observable

A backend cannot reconstruct timing information discarded upstream. The original
timestamped sender resampler generated its output PTS from an initial time plus its
nominal output sample count. Comparing RTP sample progression to that same
SR-derived nominal time can therefore report exactly 48 kHz while original
device-position/QPC progression differs. This is a **design inference from the
source**, not a measurement that a particular endpoint drifts. That sender
element can eventually declare a discontinuity on accumulated timestamp/sample
disagreement; adding receiver ASRC alone does not remove that trigger.
[Pinned sender resampler](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c#L660-L691),
[output timestamp construction](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c#L783-L981).

Before live correction, demonstrate independent observations pairing original
device sample positions with their mapped shared-clock timestamps, plus their
relationship to the transmitted PCM sample sequence. Proposed minimum anchor:

- Session/generation, stream identity, anchor sequence, and associated SSRC.
- Original device-frame position and nominal device rate.
- Original mapped first-sample capture time, with clock-health/calibration epoch.
- The conversion segment origin and exact rational association to the extended
  48 kHz wire-sample index; do not round a source-rate conversion per packet.

Retain original anchors unchanged. A fixed nominal conversion preserves the
fractional source-rate error only if it does not insert/drop frames or reset its
phase; demonstrate that condition rather than assume it. Packet sequence alone
is not a sample index when packet sizes vary. The experimental
[original-anchor wire contract](audio-anchor-wire-draft.md) now carries these
records separately; it does not mislabel monotonic timestamps as UTC.

The **offline ASRC harness with independently generated anchors**, conversion
observability fixture, and timestamp-free nominal converter are now implemented
and tested separately. The bounded [original-anchor transport diagnostic](audio-anchor-transport-validation.md)
also preserves original metadata and its exact nominal sample association.
Live correction remains disabled: the owned-PCM correction worker now has an
offline fixture and model-based phase guard. Changing-ratio signal-quality/resource
gates, source-timestamp uncertainty and presentation scheduling still need measurements.
No silent timestamp rewrite is approved.

## Receiver placement and one correction owner

Proposed order:

`validated RTP/L24 -> ordered PCM + independent capture anchors -> F32 stereo -> ASRC worker -> shared-clock 48 kHz timeline -> existing bounded presentation queue -> IPC/OBS adapter`

Network reordering/recovery finishes before ASRC. Arrival times determine whether
data can still meet a deadline, **never** the sample-clock estimate. Use original
sample counts before concealment or ASRC. Do not send audio through a hardware
playback sink merely to obtain rate matching. The sender's nominal format
conversion is not another adaptive controller. No simultaneous OBS async-audio
filter, sink-clock slaving, or queue-occupancy rate servo is assumed.

The receiver worker is the only rate-command owner for this stream. It owns one
resampler state, the existing `SampleRateEstimator`, an output sample counter,
and a bounded input/anchor ledger. A future microphone requires its own device
estimate and state. It must not inherit the desktop oscillator estimate.

## Initial control policy, not an unbounded PLL

For original device rate error `e` in ppm, the nominally converted input retains
that error, provided gate zero passes. The requested ratio is:

`r = output_frames / input_frames = 1 / (1 + e / 1,000,000)`

A fast input needs `r < 1`; a slow input needs `r > 1`. Keep the 48 kHz output
clock fixed. Candidate milestone limits are +/-500 ppm, matching the core's
default; a clamped out-of-range estimate is a fault indication, not permission
to keep playing indefinitely.

Start with the existing 1-second nonoverlapping estimator windows. Require three
consistent valid windows before priming output; discard old priming PCM rather
than retaining three seconds or moving its timestamps. Anchor the accepted new
output generation from fresh capture metadata. Initial ratio can be set before
any output; no audible ratio step is needed at startup.

While locked, ramp the command at no more than **100 ppm per output second**,
using a nominal 480-frame output quantum (at most approximately 1 ppm change per
10 ms). These are proposed test parameters, not established perceptual limits.
Use generated output time, not timer wakeups, to advance the ramp. Do not let
repeated partial/no-progress calls advance it. Preserve an unfinished quantum's
target and account for the library's actual ratio interpolation.

The first milestone is feed-forward correction plus a phase-error monitor, not
a second queue-level PID. Record disagreement between the reconstructed source
timeline and fixed output grid. Candidate hard failure is more than 10 ms,
invalid/backward anchors, an out-of-range slope, stale clock/SR/anchor health,
or an expired input deadline. Flush and re-prime a new generation instead of
dropping/repeating occasional samples or rapidly changing pitch to catch up.
If gradual bias needs an active phase servo, design it within this one controller
and validate it separately; feed-forward alone is not a proof of indefinite lock.

## Concrete libsamplerate worker contract

Create `src_new(SRC_SINC_BEST_QUALITY, 2, ...)` once on an ordinary worker thread;
fail clearly if the installed build lacks it. Use interleaved F32 input/output
and disjoint preallocated arrays. `src_process` receives input availability,
output capacity, a finite positive ratio inside the project limit, and
`end_of_input=0` for a live segment. Advance only by its reported
`input_frames_used` and `output_frames_gen`; retain unconsumed input.
[Streaming API](https://libsndfile.github.io/libsamplerate/api_full.html).

Use `SRC_DATA.src_ratio` for ordinary updates, not repeated `src_set_ratio`
calls. The pinned stereo sinc implementation interpolates from the preceding
ratio using generated output versus the requested output capacity; it saves the
actual reached ratio, not necessarily the target. Thus partial processing is
part of the controller contract, not an implementation detail to ignore. Do not
promise block-size-invariant ramping before testing it.
[Pinned interpolation](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/src_sinc.c#L546-L630).

Use checked size/frame arithmetic before the API; reject nonfinite PCM, malformed
buffers, and unsupported caps. Do not silently alter gain or clip float output.
Decode packed L24 through the existing GStreamer conversion path, retaining
timestamp provenance in the separate ledger. Check peak/headroom after ASRC;
matrix headroom alone does not guarantee the filtered waveform stays in range.

Treat an empty call as no progress, not a success loop. At most eight processing
calls and 3,840 produced frames per worker dispatch; return to control handling
when either bound is reached. No worker call, allocation, or wait runs in an OBS
render/audio callback. On a real segment end, bounded EOS drain is allowed only
for samples still belonging to that segment and meeting their deadlines. On
clock failure, restart, privacy cutoff, or discontinuity, discard history with
`src_reset` and reset the ledger instead of draining old media into the new epoch.
[Reset contract](https://github.com/libsndfile/libsamplerate/blob/0.2.2/include/samplerate.h#L122-L129),
[reset implementation](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/samplerate.c#L249-L266).

## Timestamps, look-ahead, memory, and privacy

The library processes samples and owns no `GstBuffer` timestamps. Never label
the first produced sample with the most recently supplied input-buffer PTS.
Maintain two explicit concepts: immutable original capture anchors, and the
uniform shared-clock sampling times of the reconstructed output signal.

For an established capture origin `T0`, output index `n` uses
`RationalTimeline(T0, 48000).at(n)`. Final presentation applies the existing
`CaptureToPresentation` mapping exactly once. Filter look-ahead determines when
the first output can be computed, not an automatic extra timestamp offset.
Impulse/marker fixtures must establish the correct signal origin and any
algorithmic boundary treatment before this mapping is accepted. No public API
exposes the exact contributing source position of every resampled output frame;
do not fabricate that provenance from `input_frames_used` alone.

All processing latency and reserves fit **inside** the existing total delay
budget, not an additional two seconds. Keep the resampler close to the bounded
presentation scheduler: candidate local pending-input limit is 200 ms (9,600
stereo F32 frames, 76,800 bytes) and output staging is at most 80 ms (3,840
frames, 30,720 bytes). The larger timestamped media reservoir remains singly
owned outside the DSP; do not duplicate it in an unbounded GStreamer appsink or
library feeder. Limit anchors by count and age as well as bytes. Overflow or
deadline expiry drops the invalid generation, not silent time stretching.

Library state is additional memory: profile it and its allocations rather than
claiming these application caps bound the whole process. The pinned sinc path
allocates its internal history at creation and reset clears it; the target build
still needs allocation tracing under ratio changes, partial input, and reset.
[History allocation/reset](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/src_sinc.c#L192-L307).
Proposed isolated-worker gates: under 8 MiB incremental steady state, no growth
with duration, no steady-state allocation, and p99 processing under 2 ms per
10 ms stereo output quantum on the test host. Report hardware, build, maximum
latency and scheduling misses; these are targets, not current measurements.

Desktop-only first. Later microphone support must flush **resampler history**,
queued input/output, and the capture-time ledger on its privacy epoch, in
addition to the existing delivery-time mute gate. Already submitted downstream
audio cannot be retracted by resetting this worker. No multi-second future mic
submission, or guarantee about downstream filter queues, follows from this design.

## Required finite fixtures before any live activation

All thresholds below are proposed acceptance gates, not library specifications.
Keep generated audio and detailed run logs outside the public repository.

| Gate | Fixture and pass condition |
|---|---|
| Timing observability | Generate independent device positions and shared-clock times at 0, +/-100 and +/-500 ppm, including the sender's source-rate conversion. Recover the original slope, not tautological nominal RTP progression. Vary input/packet sizes and cross the sender's current discontinuity threshold in simulated time. Missing anchors must keep correction disabled. |
| Backend sign/count | Feed a continuous-time tone and nonuniform markers sampled at each known source rate. Apply the known correct ratio without an estimator. After defined boundary guards, no duplicate/missing marker, marker error at most one output sample, and output duration/count agrees with an independent continuous-time oracle within the explicitly documented endpoint rounding. |
| Controller | Repeat at least 600 seconds simulated duration for each sign/rate; add 0 -> +500 -> -500 ppm steps and a slow ramp. Compare ground-truth slope, commanded ratio, source/output phase and queue occupancy. After acquisition, require <=5 ppm steady rate error and <=5 ms marker phase error, with no resets in the clean fixture. A long soak remains a separate gate. |
| Chunking/slew | Use fixed and randomized 1..1,920-frame inputs, partial output capacity, zero-progress calls and identical output-index rate schedules. Same marker count and <=1-sample relative marker phase across chunkings; no command slew faster than the declared bound. Partial calls must not accelerate the ramp. |
| Signal quality | Silence, DC, impulses, stereo-isolated 1/10/18 kHz tones and a multitone with measured headroom. All values finite; no channel leakage above -100 dBFS on isolated fixtures; tone gain within 0.2 dB after guards; continuous-tone residual relative to the time-warp oracle below -80 dB RMS with no local residual peak above -50 dBFS. These thresholds must be measured, not inferred from a quality preset. |
| Faults and epochs | Wrong/reused generation, backward or corrupt anchors, >500 ppm, timestamp overflow, clock/SR/anchor timeout, packet reorder/duplication/loss, 250 ms stall, repeated reset and fractional initial timestamp. No arrival fallback, old-generation replay, unbounded drain, queue growth, or artificial clock-rate response to arrival jitter. Declared silence is not missing input. |
| Resource bounds | Trace allocations, peak memory, call duration, dispatch budgets and queue caps across all fixtures. Inject output stalls and allocation failure at initialization. Failed initialization is explicit, not a unity-ratio fallback advertised as working ASRC. |
| Later integration | Only after preceding gates: timestamp-preservation transport test, then isolated encoded OBS tests and physical nonuniform A/V markers across restarts and sustained load. Synthetic successes do not satisfy these gates. |

Use separate counters for received/consumed/produced frames, priming discards,
late drops, reset reason, ratio saturation, no-progress calls and stale anchors.
Log aggregate timing/peak/resource diagnostics, not endpoint identities or PCM.
The [offline worker/harness report](audio-correction-validation.md) now records
the initial subset; [phase-guard evidence](audio-phase-validation.md) extends it.
Variable-ratio quality, uncertainty and resource measurements are next; they must not change startup services,
normal OBS sources, mute macros, or the current production audio route.
