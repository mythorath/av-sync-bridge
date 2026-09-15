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
- [ ] Physical V4L2 capture, sequence/time validation and stable calibration.
- [ ] Smooth per-input ASRC, with one adaptive rate controller per path.

The two completed transport subsets above do not complete the broader capture
or clock-uncertainty gates. No microphone, physical video, adaptive drift control,
RTX, OBS network handoff, or production startup migration is included yet. See
[network validation](network-validation.md) for actual scope and known gaps.

If the shared timestamp relationship does not survive either capture or OBS
delivery, stop and revise that boundary. Do not disguise failure with a new offset.

## Milestone 3 — production readiness

- [ ] Three unchanged unique-event runs: median offset <= one frame and every
  marker <= two frames at 60 fps on the **physical end-to-end** path (the synthetic
  subset passes; this production gate is still open).
- [ ] Thirty-minute representative load: no growing drift, unbounded memory,
  popping or unexplained late concealment.
- [ ] Source, sender, service, OBS and both-machine restart matrix.
- [ ] Mic filter/mute privacy behavior and separate mixer controls verified.
- [ ] Device discovery, packaged builds, configuration validation and diagnostics.
- [ ] Hidden startup, readiness checks, versioned migration and rollback.
- [ ] 4K60 cadence, color/range correctness and measured CPU/RAM/GPU headroom.

Synthetic results cannot satisfy the physical-capture or network-clock gates.
Do not advertise automatic sync for arbitrary GoPro, phone, RTMP or HDMI sources.
