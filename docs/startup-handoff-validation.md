<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Bounded startup and corrected desktop handoff

Checkpoint: 2026-09-15. Experimental desktop-only integration; not a production
replacement, physical A/V calibration, restart certification or long-load soak.

## Startup change

The old rule restarted acquisition whenever three one-second rate estimates
disagreed by more than 20 ppm. This could indefinitely defer startup despite
each estimate satisfying the correction range; one prior run took 19.24 seconds.
The worker now takes the median of the first three **valid** original-clock
windows and treats it as a provisional initial ratio. Spread is reported, not
hidden. This deliberately replaces the old agreement heuristic; it is not proof
of steady clock lock or absolute timestamp accuracy.

The +/-500 ppm validity gate, original-anchor provenance, health/freshness
limits, 99 ppm/s command slew, and per-sample 10 ms phase guard are unchanged.
Priming PCM is still discarded. Invalid windows still fault; no timer forces
unqualified audio out. Network-clock qualification before capture can still add
time. The deliberate two-second presentation buffer is additional latency.

Generated continuous timestamp-noise fixtures (35 and 350 microsecond amplitude)
now acquire between 3.0 and 3.1 seconds despite window spreads above 20 ppm,
continue delivering audio, and stay below 2 ms modeled phase disagreement.
Existing wrong-rate, stale, discontinuity, reset and phase-fault controls pass.

## Handoff contract

`--desktop-ipc NEW_PRIVATE_FILE` on the Linux receiver publishes corrected stereo
to the native adapter's existing IPC protocol. It is separate from the video
mapping; no video or microphone samples are published. Capture timestamps are
preserved and presentation is exactly capture + 2 seconds. An actual OBS source
would use `avsync_prototype_desktop` with its `ipc_path` set to this new file.
The command does not create that source or alter a running OBS profile.

The 224-block presentation ring is reserved/prefaulted at initialization. A
16-block owned retry queue absorbs brief reader mutex contention without waiting
inside steady-state publish calls. Dispatch attempts at most eight writes;
200 ms capture-age/overflow bounds fail closed. Faults revoke the old mapping,
including its future audio. Creation/revocation are ordinary control-thread
operations and may perform filesystem/mutex work; never use them in OBS or
capture callbacks. This finite mode stops on a new media generation; automatic
restart/reconnect remains future work. Default inspect/discard diagnostics keep
their previous generation-recovery behavior.

Unit tests verify exact float PCM and timestamps, no early delivery, splitting
larger output into 480-frame quanta, invalid sizes/NaNs/epochs/phase jumps,
revocation of queued output, and deterministic mutex contention. The contention
test holds the real shared mutex in a child, verifies the producer returns
without publishing, changes the caller's PCM, then releases the mutex and checks
the original owned copy. An expired retry revokes rather than playing late.

## Real network evidence and retained failure

The first handoff attempt stopped after about 38 seconds: its original policy
treated transient publish contention as a fatal output error. The reader had
received 3,261 blocks before disconnection. This run is a failure, not evidence
of seamless delivery. The bounded retry queue above replaces that policy.

The subsequent 60-second receiver run passed:

- Acquisition discarded 145,012 nominal frames: **3.021 seconds**, versus the
  prior 19.24-second example. Initial window spread was 73.4253 ppm.
- 2,678,400 corrected frames published (55.80 seconds); one publisher mutex
  retry, maximum seven queued retry blocks, no phase fault or jitter drops.
- Maximum modeled anchor disagreement was 51,618 ns, **not physical A/V error**.
- An independent IPC reader received 5,118 consecutive due blocks (51.18 seconds),
  with exact two-second timestamp delay, finite nonzero PCM, zero stale blocks
  and zero disconnected polls. Maximum observed delivery lateness was 3.659 ms.
- Reader peak/RMS were 0.122663/0.0395394. These are level checks, not listening
  quality or acoustic calibration. Reader/sender/receiver stop at different
  times, so their whole-process frame totals must not be equated.

The current local regression passed all 31 optimized Linux CTest entries and
five targeted Debug ASan/UBSan suites covering correction, adapter, combined
chain, IPC and handoff. Windows network CTest passed all 15 entries from a fresh
terminal; all 76 Python helper tests passed. Full sanitizer coverage also runs
in CI; third-party DSP/GStreamer libraries are not sanitizer-instrumented.

## First isolated native OBS integration

A final 55-second network run used the existing native adapter and libOBS smoke
harness on a separate virtual display. The normal OBS GUI/profile was not
attached to or changed. The harness recorded 25 seconds after eight seconds of
warmup, using its existing 40 ms audio handoff lead. Its picture was blank and
its unused microphone stream had no samples; this is **not an A/V sync test**.

- Startup discarded 144,052 nominal frames: **3.001 seconds**. Initial estimate
  spread was 64.7367 ppm. Corrected IPC publication totaled 2,451,360 frames,
  with no handoff failure, phase fault, jitter drop or unexpected restart.
- The independent due reader passed 4,653 consecutive blocks, zero stale or
  disconnected polls, and 3.452 ms maximum observed lateness.
- The native OBS desktop source consumed 2,819 blocks / 1,353,120 samples per
  channel across warmup/recording/teardown. Its counters showed zero fill,
  trimming, late skipping, invalid input, starvation and discontinuities.
- The resulting 25.023-second container decoded successfully. Its desktop AAC
  track was stereo 48 kHz, mean level -29.2 dBFS and peak -17.7 dBFS. The unused
  microphone source reported zero media blocks/output samples. Its encoded track
  measured at volumedetect's -91 dB floor; no physical microphone was captured.

This verifies real Windows loopback -> transport -> correction -> delayed IPC
-> native OBS source -> encoded desktop audio, in this isolated short trial.
It does not establish listening quality, acoustic/content-time accuracy,
simultaneous Elgato timing, 4K60 performance or production restart behavior.
The older harness's synthetic-only labels are not evidence of physical sync.

## Resume here

1. The audio-only native OBS handoff above is working in isolation. Next add
   known nonuniform events and independently check absolute mixer/content
   timestamps before changing a normal scene; ordinary music levels do not
   establish absolute timing.
2. Coordinate the actual Elgato video path and desktop path on their shared
   monotonic capture clock, apply independently measured content offsets, and
   run the nonuniform-event encoded A/V test. Do not infer alignment from packet
   counters, matching buffer sizes or the resampler's phase prediction.
3. Add the explicit provider/sender generation handshake, restart recovery and
   loaded soak. Only then plan a reversible production source/startup migration,
   preserving existing scene/mute controls and microphone privacy.

Keep the private IPC mappings, recordings and raw host logs out of this public
repository. A mapping contains recent desktop PCM, even after its writer exits;
remove disposable test mappings after all test readers/producers are stopped.
