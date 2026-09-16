<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Offline desktop correction worker

Status: **partial controller milestone, not connected to live transport or OBS**.
`avsync_audio_correction` combines the original-anchor validator and libsamplerate
backend. It accepts ordered stereo PCM with the original record belonging to
that packet. This is a reusable library and generated-only fixture, not a
background service or production replacement.

## Contract

One single-owner worker serves one explicitly authorized session/generation and
provider-clock epoch. The caller owns identity admission, packet recovery and
ordering, decoding the matching L24 packet to F32, and the clock/transport-health
gate. This component has no socket, device, thread, OBS call, arrival-time clock
estimator, gain limiter, concealment or automatic recovery. `reset()` accepts an
authorized successor; a new provider clock requires a new worker. Wrong media
epochs cannot take over the active worker.

1. Validate each original record, ordered nominal sample index and finite
   1..180-frame PCM payload using the wire policy. Repeated anchors do not create
   new clock measurements.
2. Discard priming PCM until three nonoverlapping original-clock rate windows
   agree within 20 ppm; take their median. A larger spread restarts acquisition.
   Silence is legitimate PCM.
3. Set the initial ratio before emitting samples. Establish the post-priming
   capture-grid origin from the fresh original anchor, rational nominal sample
   association and at most 50 ms of rate-based extrapolation. No arrival rebasing
   or production calibration offset is added.
4. Copy PCM into a fixed 9,600-frame ring. Dispatch only with 2,048 real input
   frames available and room for a complete 480-frame output quantum. Retain
   unconsumed input according to the backend's actual counts.
5. Move the **command** toward the original-clock estimate by at most 0.99 ppm per
   full quantum: 99 ppm per generated output second. Make at most eight backend
   calls/3,840 output frames per dispatch. Insufficient input/output room makes
   no progress and never advances the command.
6. Copy output into a fixed 3,840-frame ring. Readers request 1..3,840 frames;
   their partitions do not change DSP boundaries. Return the actual count,
   epoch, contiguous output index and rational 48 kHz capture-grid timestamp.

Fixed output capacities matter because backend interpolation depends on that
capacity. A partial backend result faults the generation instead of silently
retargeting a partial ramp. The larger input offer was tested on libsamplerate
0.2.2 at the tested rates; it is not a guarantee about every future backend. The
99 ppm/s **command** bound is not a measurement of instantaneous reached ratio.
Interpolation and exact sample provenance still need an independent
changing-ratio waveform oracle.

The reconstructed capture grid is not an OBS presentation timestamp or proof of
per-sample source provenance. Backend input consumption includes private filter
buffering; it must not be treated as content delay. Presentation scheduling and
physical endpoint calibration remain separate.

## Faults, deadlines and ownership

Invalid PCM/metadata, backward caller time, unhealthy upstream, a rate outside
the strict +/-500 ppm bound, stale/future anchors, queue exhaustion or expiry
latch failure. No clamping, unity fallback, stale retry or EOS drain occurs.
Public queued PCM and filter history are cleared; delivery stops until an
explicitly authorized new generation is primed.

Input/output queue residence and absence of DSP progress are bounded to 200 ms.
The no-progress watchdog also covers private backend history when public queues
are empty. Original anchors retain the wire policy's 250 ms age/100 ms future
limits. These safety cutoffs are not a complete per-output source-age ledger or
physical-sync assurance. `upstream_healthy` must include the caller's other
required checks; the flag does not independently verify a live clock.

Worker-owned PCM storage is fixed and value-owned; callers may immediately reuse
input buffers. Library history is additional storage. Fixed rings and dispatch
budgets do not prove allocation freedom, bounded wall time, or the complete
resource gates in [the design](asrc-design.md). This worker is desktop-only;
reset is not a tested live microphone privacy interface and cannot retract
samples already delivered elsewhere.

## Repeat the offline tests

```sh
cmake -S . -B build-asrc -DCMAKE_BUILD_TYPE=RelWithDebInfo -DAVSYNC_BUILD_ASRC=ON
cmake --build build-asrc --parallel 2
ctest --test-dir build-asrc --output-on-failure
python3 tools/run_audio_correction_suite.py \
  --executable build-asrc/avsync-audio-correction-fixture --long --jobs 2
```

The suite generates independent analytic device clocks and nonuniform smooth
pulses, then drives the real original-record validator, estimator and worker.
It supplies already nominally converted F32 PCM; it does **not** run the nominal
converter, RTP, clock provider, IPC, physical devices or OBS. Those boundaries
must subsequently be tested together.

Fourteen positive cases pair fixed 180-frame packets/480-frame reads with a
deterministic 1..180-frame packet pattern/17-frame reads. They cover 0, +/-100,
+/-499 ppm, steps 0 -> +499 -> -499 -> 0, and a -300 -> +300 ppm ramp.
Every long positive case runs 600 simulated seconds. Short cases use 12, 45 and
125 seconds respectively. Rates use 499 rather than 500 because nanosecond
quantization can put an exact boundary estimate outside the strict 500 ppm
limit; that limit has not been silently relaxed.

Each case requires all 12 threshold-detected markers in order, no missing or
extra events, <=5 ms maximum absolute marker error, <=5 ppm final command error,
finite/channel-isolated output and command/dispatch bounds. Arrival is perturbed
by a deterministic 0..4 ms pattern. Step/ramp markers include the transitions,
not just their settled tail. Paired partitions must agree within one sample.
The 60-second negative control labels -499 ppm PCM with +499 ppm anchors; it
succeeds only when measured marker phase exceeds 5 ms, not on a crash or a
missing marker.

Unit tests also cover output backpressure, small reads, wrong generations,
invalid PCM, missing input, stale time, health loss, copy-in ownership, reset
with pending audio and reset to exact silence. These generated-media safeguards
are not network loss recovery tests.

See [measured results and remaining gates](audio-correction-validation.md).
