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

## Continue here

1. Extend the basic native agreement check to qualify the actual remote wrapper
   and finite pair controller under failure:
   generated network media, sender/receiver/controller failure matrix, and an
   isolated recording that spans the restart boundary with distinct old/new sound.
2. Run the isolated real desktop/video restart matrix and representative load.
   Preserve interruption and timing failures; no offset retuning between repeats.
3. Only then perform a reversible normal-source/startup migration and physical
   calibration on that final path. Keep scene identities, controls and mute state.
4. Verify sustained 4K60 cadence, color/range and resource headroom. Mic remains a
   separate later task.

Read [process-pair control](process-pair-control.md), [IPC](ipc.md),
[same-sender recovery](desktop-recovery.md), and [the roadmap](roadmap.md).
Site configuration, credentials, recordings and deployment details stay private.
