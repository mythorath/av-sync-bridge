# Roadmap and release gates

There is no production release yet. A checked item means the named milestone is
implemented and tested as described, not that physical A/V synchronization is
proven. Record evidence separately in validation reports.

## Milestone 1 — synthetic timing foundation

- [x] Portable timestamp arithmetic, rational cadence and bounded queue policies.
- [x] Deterministic tests for rate estimates, discontinuity and privacy cutoff.
- [x] Linux bounded IPC with a finite synthetic video/stereo/mic producer.
- [x] Native OBS synthetic adapter with separate video/stereo/mono source types
  (loading and recording tested; complete controls/privacy validation outstanding).
- [x] Isolated encoded-output measurement with non-uniform marker spacing
  (three synthetic repeats pass; separate absolute raw-mixer checks included).
- [x] Synthetic staggered start, video hide/show, producer replacement, 400 ms
  producer pause and rapid mute fixtures (not the full production restart/privacy matrix).
- [x] Six-cycle, 119-second synthetic check with all 36 markers per audio track.
- [x] Continuous isolated recording across graceful and hard-crash producer
  replacement, with distinct queued-old/new audio and picture identities and
  full-trace absolute timing checks. See [restart recording validation](restart-recording-validation.md).

See [handoff validation](handoff-validation.md) and the preserved
[initial failures](validation-2026-09-15.md). The synthetic foundation is ready for
isolated real-capture development, not production deployment.

## Milestone 2 — real capture and clock transport

- [x] Explicit Windows loopback metadata probe: existing mix format, device/QPC
  timestamp pairs and bounded diagnostics; no PCM storage or network output.
- [x] Optional bounded desktop-only WASAPI sender, explicit stereo conversion,
  shared-monotonic clock mapping, and RTP/RTCP diagnostic receiver.
- [x] Actual eight-channel conversion fixtures on Windows/Linux; short real-network
  starts, one sender-clock outage/reacquisition and sender SR correspondence checks.
- [ ] Independently measured WASAPI content-time calibration and microphone capture.
- [ ] Shared-clock uncertainty and generation behavior under representative load.
- [ ] RTP/RTCP PCM and bounded RTX interoperation, including loss/reordering tests.
- [x] Explicit physical NV12 V4L2 capture, checked sequence/time metadata and
  bounded video-only IPC handoff with in-memory delivery verification.
- [ ] Capture startup readiness, loss-free loaded delivery, color interpretation
  and stable physical content-time calibration.
- [x] Optional bounded stereo resampling backend and original capture-anchor
  metadata gate, tested offline with analytic signals and wrong-rate controls.
- [x] Offline sender-conversion observability test demonstrating hidden drift
  and timestamp phase steps on tested Windows/Linux runtimes.
- [x] Timestamp-free nominal sender converter with independently checked sample
  counts, phase, partition invariance and restart behavior on Windows/Linux.
- [x] Bounded desktop original capture-anchor wire diagnostic, exact calibration
  provenance, ordered PCM association and finite generation admission.
- [ ] Automatic provider/sender restart handshake, loaded loss/reordering/recovery
  and sustained original-anchor uncertainty characterization.
- [x] Finite process-agreement fixture with pinned sender identities, stdin
  control leases, bounded pair-restart policy and explicit IPC-v2 retirement.
  Generated tests qualify these software boundaries only; live restart, remote
  wrapper fencing and installed startup remain open. See [process control](process-pair-control.md).
- [ ] Smooth per-input ASRC, with one adaptive rate controller per path.
- [x] Offline desktop feed-forward worker: provisional rate acquisition, fixed-quantum
  command slew, owned bounded queues, original-anchor origin and explicit reset.
- [x] Model-based runtime phase guard, pinned backend audit, independent linear-PCM
  phase check and fault/no-false-alarm fixtures with generated media.
- [x] Variable-ratio sound-quality fixtures, worker allocation/failure audit and
  individual-call paced CPU measurements (affinity-conditioned pass on test host).
- [x] Combined generated 192 kHz conversion/RTP/correction marker checks and
  bounded desktop-only live inspect-and-discard adapter; no OBS output.
- [x] Bounded three-window startup and explicit corrected desktop IPC handoff,
  with due-reader verification, bounded contention retry and fault revocation.
  See [startup/handoff scope and next steps](startup-handoff-validation.md).
- [x] Actual Windows desktop audio through the native source to a short encoded
  recording in isolated libOBS; no physical video, real mic or normal-profile migration.
- [x] Separate physical video/desktop IPC inputs and full-frame fitting in the
  isolated recorder; first real combined recordings and strict six-event analysis.
  Later source-policy-v2 testing passed three unchanged-setting physical timing
  runs. This does not qualify the production or reliability gates; see
  [retained failures, passes and follow-up](physical-combined-validation.md).
- [x] Opt-in same-sender clock-loss recovery with retired-generation fencing,
  fresh IPC replacement, and one physical planned-pause/OBS reconnection test.
  Recovery includes a measured silence gap; whole-process/reboot recovery and
  loaded physical calibration remain open. See [recovery scope](desktop-recovery.md).
- [ ] Loaded clock/recovery repeatability, source-timestamp uncertainty and
  complete resource qualification before a production or OBS correction path.

Completed subsets above do not complete the broader capture or clock-uncertainty
gates. No live microphone path, RTX, reliable continuous combined A/V handoff,
or production startup migration is included yet. See [network validation](network-validation.md)
and [physical video validation](video-validation.md) for actual scope and faults.
The [ASRC design](asrc-design.md) first requires original device-clock anchors to
survive conversion; nominal RTP progression alone cannot satisfy that gate.
See [offline DSP evidence](asrc-validation.md), the measured old
[conversion failure](conversion-timing.md) and the
[nominal conversion replacement](nominal-audio-validation.md). A tested resampler is not a completed
live rate controller. The [original-anchor transport milestone](audio-anchor-transport-validation.md)
preserves the timing relationship. The [bounded correction worker](audio-correction.md)
now also drives ASRC from actual desktop packets in an isolated diagnostic.
See [quality/resource and live results](audio-live-correction-validation.md);
the broader live controller gate remains open.

If the shared timestamp relationship does not survive either capture or OBS
delivery, stop and revise that boundary. Do not disguise failure with a new offset.

## Milestone 3 — production readiness

- [ ] Three unchanged unique-event runs: median offset <= one frame and every
  marker <= two frames at 60 fps on the **production physical end-to-end** path.
  Three isolated physical runs passed these limits; the normal-profile production
  migration and its repeat verification are still open.
- [ ] Thirty-minute representative load: no growing drift, unbounded memory,
  popping or unexplained late concealment.
- [ ] Source, sender, service, OBS and both-machine restart matrix.
- [ ] Mic filter/mute privacy behavior and separate mixer controls verified.
- [ ] Device discovery, packaged builds, configuration validation and diagnostics.
- [ ] Hidden startup, readiness checks, versioned migration and rollback.
- [ ] 4K60 cadence, color/range correctness and measured CPU/RAM/GPU headroom.

Synthetic results cannot satisfy the physical-capture or network-clock gates.
Do not advertise automatic sync for arbitrary GoPro, phone, RTMP or HDMI sources.
