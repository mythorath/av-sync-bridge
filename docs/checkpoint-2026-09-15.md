<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Development checkpoint — 2026-09-15

This is a resumable experimental checkpoint, not a production-ready release.
No existing production audio path has been migrated to AV Sync Bridge.

## Saved progress

- Separate physical video and corrected desktop-audio IPC inputs feed the
  isolated OBS recorder. Video publication, generation retirement, bounded
  duration rejection and original-clock diagnostics have added regression tests.
- Opt-in same-sender recovery has one physical planned-pause/reconnection pass,
  with a measured silence gap. It does not restart sender/receiver processes.
- The browser reference produces a finite nonuniform six-marker sequence;
  policy v2 retains timing-query, mapping, draw, visibility and clock-progress
  checks. The physical analyzer rejects incomplete or ambiguous passes.
- Three unchanged-setting isolated physical timing runs passed. Median
  audio-minus-video offsets were +1.000, +4.333 and -2.000 ms; all 18 events were
  within one nominal 60-fps frame. Detector quantization and uncertainty still
  apply. See [full results and retained failures](physical-combined-validation.md).
- A separate host-local OBS shutdown repair passed generated-media recording
  and two ordinary close/restart checks. It is not an official OBS fix, a bridge
  source change or a portable deployment artifact; private rollback details stay
  outside this repository.

## Continue here

1. Recheck the current running state before changing anything. Preserve existing
   scene identities, controls, audio routes and mute privacy. Microphone work is
   deferred; there is no real-microphone calibration result.
2. Implement and qualify the explicit provider/sender process handshake and
   supervised whole-process recovery before claiming restart robustness.
3. Run the remaining representative-load soak and restart matrix in an isolated
   setup; keep failures and compare unique events without retuning between runs.
4. Plan a reversible production source/startup migration, then repeat physical
   calibration on that actual path. Existing isolated timing results do not
   automatically transfer to a normal OBS scene or another capture format.
5. Qualify sustained 4K60 output cadence, color/range and resource headroom.
   The physical timing recordings were 640x360 at 60 fps; a short generated 4K60
   recording is not a sustained performance test.

Reference documents: [startup/handoff](startup-handoff-validation.md),
[same-sender recovery](desktop-recovery.md), [browser reference](browser-reference.md),
[physical measurement](physical-measurement.md), and [roadmap](roadmap.md).

Private recordings, raw timing reports, live profiles, deployment scripts and
device/account identifiers are deliberately excluded from the public repository.
