<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Model-based audio phase guard

Status: **offline-tested runtime safeguard, now used in the optional live desktop
inspect-and-discard diagnostic; no OBS output**. See
[live boundary evidence](audio-live-correction-validation.md).
The worker checks every proposed output sample against original capture anchors
before calling the DSP or exposing that quantum. This monitors timing consistency,
not acoustic events or the correctness of capture metadata.

## Model and ledger

`SincPhaseModel` predicts nominal input position per output sample. It does not
read private library state or infer positions from consumed-input counts. The
model is restricted to **libsamplerate 0.2.2 stereo sinc**, full 480-frame non-EOS
calls, reset before a segment, and +/-500 ppm. The worker constructor rejects
other reported versions; the low-level backend remains available independently.
Version text is necessary, not proof that a vendor has not patched its build.

For initial ratio `r0` and requested ratio `rt`, sample `i` uses
`r(i) = r0 + (rt-r0)*i/480` when the difference exceeds the audited deadband.
The source cursor advances by `1/r(i)`; the next call starts at the last reached
ratio, not automatically the target. Reset starts at the segment's source origin.
This mathematical model is original code, not a copied filter implementation or
a dependency on private ABI layout. See the pinned
[stereo implementation](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/src_sinc.c#L546-L627)
and [processing/reset implementation](https://github.com/libsndfile/libsamplerate/blob/0.2.2/src/samplerate.c#L80-L135).

An integer whole position plus a fraction preserves precision at large device
counters. Each generation has a **24-hour generated-output horizon**, then fails
closed and needs an authorized successor. This experimental limit is explicit;
there is no automatic recovery.

`AudioPhaseLedger` holds at most **512 original anchors** and prunes only records
older than the earliest source position still needed by DSP. An interval spans
at most **100 ms**. Every predicted sample needs bracketing anchors. Their
interpolated capture time is compared to the fixed output grid, without arrival
time, invented anchors, extrapolation or rewriting the grid to conceal error.

Missing brackets make no progress: no DSP call, model advance or command advance.
Existing health/200 ms stall deadlines still apply. Invalid/gapped/full ledgers,
model-horizon exhaustion or absolute predicted phase above **10 ms** latch a
fault, clear queued PCM/filter/ledger/model state, and withhold the failing
quantum. Already delivered audio cannot be recalled. Extremely dense valid
anchors can exhaust this ledger and are rejected, not buffered without limit.

## What this does not prove

The guard compares an audited **model** to **reported** timestamps. It cannot
discover a coherent lie shared by all timestamps, unmeasured device delay,
incorrect sender sample association, or sound preceding its capture timestamp.
The wrong-anchor control still demonstrates this blind spot: the guard sees
consistency while the independent marker oracle detects drift. Whole-path
calibration remains mandatory; this is not universal automatic lip sync.

Interpolation assumes sufficient anchor accuracy/density. The 100 ms cap does
not bound within-interval clock uncertainty. This is not a certified per-sample
uncertainty interval or arbitrary-library guarantee. Generated-waveform agreement
does not establish all-band sound quality, deadlines, mic privacy or live recovery.

## Independent checks

`avsync-asrc-phase-probe` sends linear PCM ramps through the real resampler and
decodes fractional input position from the resulting samples independently of
the recurrence. Two periods and wide startup/wrap guards cover both channels.
This measures **local sub-sample phase**, not whole-cycle correctness; the
separate nonuniform-marker matrix remains necessary. A wrong unity model must
disagree with actual +499 ppm processing.

The controller fixture's `cycle` case repeats clock-speed transitions without
overriding the production controller. Feed-forward lag accumulates until the
guard must stop it. Units also inject an 11 ms timestamp shift, withhold brackets,
cross large integer positions, fill/prune the ledger, exceed time/index limits
and check clean reset after a phase failure.

```sh
cmake -S . -B build-asrc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVSYNC_BUILD_ASRC=ON
cmake --build build-asrc --parallel 2
ctest --test-dir build-asrc --output-on-failure
build-asrc/avsync-asrc-phase-probe --seconds 600
build-asrc/avsync-audio-correction-fixture \
  --scenario cycle --seconds 300 --expect-phase-fault
python3 tools/run_audio_correction_suite.py \
  --executable build-asrc/avsync-audio-correction-fixture --long --jobs 2
```

See [measured results and remaining gates](audio-phase-validation.md).
