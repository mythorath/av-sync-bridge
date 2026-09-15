<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Bounded stereo ASRC backend

Status: **optional offline-tested DSP component, not a live A/V sync fix**.
This implements the backend portion of [asrc-design.md](asrc-design.md).
It does not estimate clock drift, preserve sender timing anchors, assign output
timestamps, queue network media, change devices, or connect to OBS. The timing
observability and physical/encoded A/V gates in that design remain separate.

## Build and API

Enable `AVSYNC_BUILD_ASRC=ON` with the libsamplerate development package available
through `pkg-config` (`samplerate`). The default portable core has no new runtime
dependency. The optional `avsync_asrc` target exports `avsync/asrc.hpp` and uses
libsamplerate `SRC_SINC_BEST_QUALITY`, two channels, interleaved F32. Initialization
fails explicitly if that converter cannot be created; there is no fallback.
Original project code remains GPL-2.0-or-later. The linked library has its own
[BSD-2-Clause notice](https://github.com/libsndfile/libsamplerate/blob/0.2.2/COPYING).

```cpp
avsync::AsrcStereo converter(initial_device_error_ppm);
const auto result = converter.process(input_floats, output_floats,
                                      requested_device_error_ppm, false);
// Advance input by result.input_frames_used * 2 floats.
// Use only result.output_frames_generated * 2 output floats.
// Return to worker control handling instead of spinning on no_progress.
```

The class is single-worker-thread-owned, noncopyable and nonmovable. Allocate its
state and caller-owned arrays before running. `process` makes at most one library
call and does not itself allocate, lock, perform I/O, or retain the caller's
spans. The library copies required sample history into its own state. The
wrapper's input availability is capped at 9,600 frames, output capacity at 3,840
frames. At stereo F32 those are 76,800 and 30,720 bytes; these caps do **not** bound
library state or the whole process. The future worker still needs a per-dispatch
budget (design: eight calls and 3,840 generated frames), deadlines and scheduling
measurements. A frame count cap is not a hard real-time wall-clock guarantee.

Buffer lengths are float counts and must be even. Input/output must not overlap;
oversized, malformed or nonfinite input is rejected before changing state. A
finite command must be within +/-500 ppm and maps to output/input ratio:

`1 / (1 + device_error_ppm / 1,000,000)`

A positive error reduces the number of output samples. Commands outside the
limit are rejected, never clamped or silently converted to unity. Finite float
PCM is not gain-normalized or clipped; valid output can exceed +/-1. The caller
must establish headroom and validate signal quality. Unexpected backend counts,
backend errors or nonfinite output latch a failed state, clear the supplied
output array and expose no output as valid. Discard that generation and reset;
do not retry the old PCM as though the failed call had never consumed history.

## Progress, segment end and reset

The result contains `status`, `input_frames_used`, `output_frames_generated` and
`library_error`. `progress` means at least one actual count is nonzero; consuming
input without output is legitimate filter priming. `no_progress` means both
counts are zero. `invalid` means validation failed without changing backend
state. `failed` requires a new generation/reset. `finished` means a deliberately
ended segment has completely drained.

Empty output or empty non-EOS input returns `no_progress` without calling the
library or retargeting its ratio. Thus an empty live call does not drain queued
library samples: supply the next real packet, or deliberately end the segment.
Keep unconsumed input until the next call; never advance by its supplied size or
equate library input consumption with the source position of an output sample.

For a true finite segment, set `end_of_input=true` on the final input, keep that
flag on calls supplying its unconsumed suffix, then make bounded empty EOS calls
until `finished`. Do not supply new unrelated PCM while draining. A non-EOS call
after draining starts is rejected. The wrapper supplies a private nonnull dummy
input pointer during an empty EOS call: the audited 0.2.2 sinc backend returns
early on a null input pointer before it can complete end padding.

`reset(initial_ppm)` discards history and clears failed/EOS state, setting an
initial ratio before any new media. Invalid ppm leaves the existing state alone.
Reset the separate sample/timestamp ledger as well. A clock failure, restart,
discontinuity or privacy cutoff must reset/discard, not drain stale audio into a
new generation. This operation cannot retract audio already submitted elsewhere.

## Partial calls and ratio smoothing

Construction and reset use `src_set_ratio` once, before media. Normal commands
use `SRC_DATA.src_ratio`; the backend interpolates from its previous actual
ratio. Its public API does not expose the exact per-output source position or
the reached internal ratio. This wrapper therefore does not invent either.
See the [library API contract](https://libsndfile.github.io/libsamplerate/api_full.html).

In the audited 0.2.2 stereo sinc path, the requested output capacity determines
the interpolation denominator. A partial call saves the last reached ratio,
not necessarily the target. Repeating the target with a new capacity changes
the subsequent transition. Even a full call's last emitted sample precedes the
mathematical end of that ramp. This is why a 480-frame call and a 3,840-frame call
with the same command are **not** a fixed-duration, chunk-invariant slew.
[Pinned stereo implementation](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/src_sinc.c#L546-L627).

The backend intentionally does not implement the proposed 100 ppm/output-second
controller. That future controller must retain unfinished-quantum state and
advance only by generated output frames, including partial/no-progress calls;
its schedule and phase error require independent fixtures. Passing a new target
once per arriving packet is not such a controller.

## Unit validation and limits

`avsync-asrc-tests` covers:

- Finite command limits, stereo sizes, maximum spans, both directions of buffer
  overlap and rejection of NaN/infinite PCM without mutation.
- Empty/no-progress calls, exact consumed/generated count handling, explicit
  bounded EOS drain, completed-segment rejection and repeated history reset.
- Constant commands at 0, +/-100 and +/-500 ppm; finite output and final count
  within one output frame of the independent rational-ratio expectation.
- Constant-ratio partial inputs of 1/19/480/1,920 frames and outputs of
  1/17/480/3,840 frames, checking count and <=2e-6 sample difference in that
  finite fixture. This does not establish variable-rate chunk invariance.
- A changing-command/partial-call schedule checked sample-for-sample against
  the direct stateful library API; a separate transition deliberately verifies
  that output-capacity changes produce different waveforms.
- Reset to silence with no old nonzero history, and finite values above unity
  remaining unclipped rather than hiding a gain/headroom problem.

An initial Linux GCC 13.3 build against libsamplerate 0.2.2 passed **431,962 checks**.
That is an offline unit result, not a perceptual-quality, allocation-free, drift
controller, physical endpoint, transport, OBS or A/V synchronization claim.
The separate offline signal/timestamp/resource fixture supplies additional
measurements; no live audio route is enabled by this target.
