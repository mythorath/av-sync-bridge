# Roadmap and release gates

There is no production release yet. A checked item means the named milestone is
implemented and tested as described, not that physical A/V synchronization is
proven. Record evidence separately in validation reports.

## Milestone 1 — synthetic timing foundation

- [ ] Portable timestamp arithmetic, rational cadence and bounded queue policies.
- [ ] Deterministic tests for rate estimates, discontinuity and privacy cutoff.
- [ ] Linux bounded IPC with a finite synthetic video/stereo/mic producer.
- [ ] Native OBS adapter with independent video and audio controls.
- [ ] Isolated encoded-output measurement with non-uniform marker spacing.
- [ ] Staggered start, scene hiding, producer restart and rapid mute tests.

## Milestone 2 — real capture and clock transport

- [ ] Capture-correlated WASAPI timestamps, preserving speaker configuration.
- [ ] Shared application clock and explicit epoch; measured uncertainty.
- [ ] RTP/RTCP PCM and bounded RTX interoperation, including loss/reordering tests.
- [ ] Physical V4L2 capture, sequence/time validation and stable calibration.
- [ ] Smooth per-input ASRC, with one adaptive rate controller per path.

If the shared timestamp relationship does not survive either capture or OBS
delivery, stop and revise that boundary. Do not disguise failure with a new offset.

## Milestone 3 — production readiness

- [ ] Three unchanged unique-event runs: median offset <= one frame and every
  marker <= two frames at 60 fps (proposed targets, not current performance).
- [ ] Thirty-minute representative load: no growing drift, unbounded memory,
  popping or unexplained late concealment.
- [ ] Source, sender, service, OBS and both-machine restart matrix.
- [ ] Mic filter/mute privacy behavior and separate mixer controls verified.
- [ ] Device discovery, packaged builds, configuration validation and diagnostics.
- [ ] Hidden startup, readiness checks, versioned migration and rollback.
- [ ] 4K60 cadence, color/range correctness and measured CPU/RAM/GPU headroom.

Synthetic results cannot satisfy the physical-capture or network-clock gates.
Do not advertise automatic sync for arbitrary GoPro, phone, RTMP or HDMI sources.
