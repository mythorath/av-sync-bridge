<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Development checkpoint — 2026-09-16

Experimental desktop-only restart work. The normal OBS/production audio path has
not been migrated, and microphone work remains deferred. Display troubleshooting
is outside this milestone; no display setting was changed for these tests.

## Implemented software boundaries

- Receiver admission can pin the expected sender process before any media arrives.
  Foreign provider/session RTP is fenced before branch/report allocation. RTCP
  must refer to an SSRC previously seen in pinned RTP. These identifiers are not
  authentication and do not protect against a hostile network.
- The receiver publishes a versioned, string-valued process agreement only after
  bounded pipeline readiness checks. The sender echoes it before clock acquisition;
  neither message claims a qualified clock, PCM delivery or OBS consumption.
- Native stdin control polls during acquisition and processing. A five-second
  lease, EOF or invalid command stops the run; buffered late keepalives cannot
  revive it. STOP acknowledges cleanup separately from media-quality statistics.
- IPC v2 binds mappings to their own inode and singleton-lock inode. Explicit
  replacement validates and retires a crashed predecessor before publishing a
  new generation. Existing readers cannot read old queued PCM after publication.
  Already copied/submitted OBS audio cannot be retracted.
- The finite Python coordinator restarts an agreed pair, not an individual process
  against cached tokens. It has a total deadline, small retry budget, OS-held local
  lock, bounded output/control queues and retained fault history. It is not a
  service installer, media-health monitor or remote process containment system.

**ABI change:** rebuild IPC producers, readers and the OBS adapter together.
Old v1 mappings are rejected, not silently migrated. No installed plugin was
replaced by this checkpoint.

## Verification and limits

The Windows Release suite passed 20/20 and the Linux ASRC/network/IPC suite passed
34/34. These include real native pipe EOF/lease tests and generated child-process
hard-death IPC cases, not live sound. IPC tests use distinguishable predecessor
and successor PCM and test abandoned mutexes, live-writer exclusion, unsafe or
copied files, old protocol, allocation failure, and busy-reader refusal.

Six targeted Linux ASAN/UBSAN tests passed with default leak checking. The video
producer, OBS adapter and isolated recorder were rebuilt against IPC v2; nothing
was installed into normal OBS. Browser-reference scheduling tests passed 27/27.

An explicit native loopback-only check passed five startup/stop/replacement cases,
including prebuffered STOP/EOF leaving the old mapping byte-for-byte unchanged
and a forced receiver death followed by fresh v2 publication. It injected no PCM.
Run it deliberately (it opens loopback UDP sockets, never capture devices):

```sh
python3 tools/check_receiver_process_control.py --run-loopback \
  --executable ./build-asrc/avsync-network-receiver
```

One isolated generated-picture/tone OBS recording passed the unchanged gates:
both generated audio tracks had median encoded audio-minus-video offset -15.333 ms
and event offsets -15.667..-15.000 ms. Raw desktop mixer onset error was
-0.605..+0.062 ms; both raw tracks passed the separate 2 ms limit. This is one
synthetic baseline, not physical calibration or a recorded restart boundary.

One finite native Windows/Linux pair over authenticated foreground SSH completed
its process agreement and cooperative stop: one attempt, one acknowledgment,
21.432 seconds, no reported control faults, forced kills or cleanup failures.
Desktop loopback and a private IPC output were explicitly selected; no normal OBS
source was attached. The controller did not analyze media summaries, so this
result does **not** establish delivered PCM, audible quality or clock lock.

An initially flaky process-fixture test was retained and investigated: stdout EOF
can precede publication of a failing process's exit status. The strict controller
then treats a nonzero STOP result as ambiguous and declines to restart. Tests now
use acknowledged-pair/exact-child-exit barriers for deterministic crash injection
and separately assert that ambiguous EOF fails closed. No production timeout or
cleanup requirement was relaxed. Windows Python discovery passed 131 tests
(7 skips); the exact problematic/ambiguity cases then passed 20 repetitions.
The Linux process-pair suite passed all 26 cases, including local lock ownership,
permissions, symlink/hardlink and FIFO checks.

The previous three isolated physical timing passes remain recorded in the
[September 15 checkpoint](checkpoint-2026-09-15.md). They have not been rerun on
this process-control path. No new physical A/V, reboot, loaded soak, microphone,
normal-profile migration or sustained 4K60 result is claimed here.

## Follow-up: recording through replacement

The [new restart runner](restart-recording-validation.md) records the interruption
itself, unlike the older warmup replacement fixture. Two graceful and two SIGKILL
runs passed with exactly A1–A3 then B1–B6 in each full recording. A4 was fully
queued before the stop and never replayed. The successor retired the old mapping
about 1.905 seconds before A4 was due. No timing offsets were retuned.

The final two runs used independently owned Xvfb and libOBS processes, avoiding
a wrapper-cleanup blind spot discovered during review. Both exited cleanly.
Their desktop encoded medians were -14.333/-2.333 ms (before/after graceful stop)
and -13.333/-2.333 ms (before/after SIGKILL). All markers passed the fixed one-frame
median/two-frame event gates, and raw mixer errors were within 0.612 ms of the
original intended presentation times. These are generated 640x360/60 recordings,
not physical capture, remote recovery or sustained 4K60 validation.

A fresh 119-second, six-cycle generated baseline also passed: all 36 markers on
both generated tracks, encoded median -15.333 ms and range -15.667..-15.000 ms;
raw desktop error -0.553..+0.114 ms. No prior/later raw marker regions were ignored.
This short check does not replace the representative 30-minute load gate.

The controller now retains bounded, privacy-filtered final native summaries,
separately from process-agreement evidence. A quiet-desktop trial completed the
agreement but reported zero captured/published frames and correctly remained
media-unqualified. A subsequent playing-desktop trial reported 5,213 outgoing RTP
packets and 685,440 corrected stereo frames published to private IPC, with no
reported timestamp, anchor, jitter-late or ingress-validation errors. That first
live report revealed an interpreter bug: the receiver's success code is the
literal `none`, not an empty string. The strict parser and its fixture were
corrected to the native contract; missing, empty and unknown values do not pass.
These counters are native self-reports, not independently observed playback or
OBS consumption. No normal OBS source was attached to those network trials.

A fresh trial after that parser fix reported both roles qualified: 5,174 outgoing
RTP packets, 677,280 corrected stereo frames delivered/published, 17 usable anchor
measurements, one running correction session with fault code zero and IPC failure
code `none`. It completed one acknowledged pair in 21.432 seconds, with no control
faults, forced kills, stop or cleanup failures. `media_verified` correctly remains
false: no independent content-quality or actual OBS-receipt assertion was made.

Unexpected remote receiver/SSH exit or stdout loss now latches
`remote_retirement_unverified` and prevents a new attempt. This closes the case
where an already-dead SSH child previously escaped the forced-kill retry guard.
It is deliberately conservative; independent authenticated remote retirement
proof and the real remote failure matrix are still unimplemented.

The expanded Linux ASRC/network/IPC CTest suite passed 35/35; four targeted
ASAN/UBSAN checks passed with default leak detection. Windows CTest passed 20/20.
Final Python discovery passed 177 tests on each host (7 Windows/6 Linux skips);
the process-pair subset contains 39 tests. The new analyzer, queue
barrier, display ownership and native-report negative fixtures are included in
the ordinary Python test discovery. No production profile, service, startup task,
microphone state or display configuration was changed.

## Continue here

1. Extend the basic native agreement check to qualify the actual remote wrapper
   and finite pair controller under failure:
   generated network media and sender/receiver/controller failure matrix with
   independent authenticated remote retirement proof. The local generated
   restart-spanning recording is now covered; it does not prove remote containment.
2. Run the isolated real desktop/video restart matrix and representative load.
   Preserve interruption and timing failures; no offset retuning between repeats.
3. Only then perform a reversible normal-source/startup migration and physical
   calibration on that final path. Keep scene identities, controls and mute state.
4. Verify sustained 4K60 cadence, color/range and resource headroom. Mic remains a
   separate later task.

Read [process-pair control](process-pair-control.md), [IPC](ipc.md),
[same-sender recovery](desktop-recovery.md), and [the roadmap](roadmap.md).
Site configuration, credentials, recordings and deployment details stay private.
