# Bounded network receiver diagnostics

The optional Linux `avsync-network-receiver` provides a shared monotonic clock
and inspects desktop PCM received through RTP/RTCP. Its default inspect/discard
mode never opens an audio device, saves PCM, changes OBS, or publishes IPC.
The explicit optional `--desktop-ipc` mode instead publishes corrected PCM to a
private buffer; see [handoff](startup-handoff-validation.md). This is a
transport/timestamp experiment, not an audible or physical A/V validation.

```text
avsync-network-receiver --bind LOCAL_IPV4 --peer SENDER_IPV4 --clock-port CLOCK_PORT --rtp-port RTP_PORT --rtcp-port RTCP_PORT --seconds 30 --expect-media --expect-anchors
```

Use a specific local unicast IPv4 address on an explicitly trusted private link,
the expected sender IPv4, and three distinct unused ports in 1024..65535. The
receiver does not change firewall rules. Media input binds only to that address,
disables socket reuse and multicast, and rejects packets with missing/mismatched
sender-address metadata. The clock provider binds the same local address but
does not authenticate clients. IP filtering is not authentication: no encryption,
tamper resistance, congestion control or hostile-network safety is claimed.

The provider clock is checked against Linux CLOCK_MONOTONIC. The receiver uses
that clock, pipeline base time zero and no automatic start-time rebasing.
PT96 is L24 stereo at 48 kHz. RTP jitter-buffer latency is 100 ms with late
dropping; this is not the eventual multi-second synchronization buffer. RTCP
sender reports follow the project's explicit **shared-monotonic, not UTC**
convention. See [clock contract](network-clock.md) and [sender](windows-sender.md).

Original-anchor mode sets each jitter buffer's `faststart-min-packets=2` while
retaining the 100 ms latency and drop-on-latency limit. This prevents its normal
startup wait from filling the queue and discarding frame zero on the tested
bursty input. It does not remove the capacity limit or promise recovery when
the consumer stalls. The receiver inspects media immediately; presentation
buffering remains a separate future stage. See
[GStreamer's startup and capacity properties](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html).
The current sender's references describe its **nominal sample-count media
timeline**, not original device-clock anchors. The receiver explicitly reports
`capture_timing_verified=false`. It must not use those references as a surrogate
for original capture timestamps or as an adaptive-rate command.

With `--expect-anchors`, a separate versioned RTP extension carries original
WASAPI device positions and mapped QPC times. Copy the fresh decimal `clock_epoch`
from the receiver's READY line into the sender's required `--clock-epoch` option.
Restarting the receiver requires a new token and a new sender invocation. This
manual finite-run agreement does not implement authentication or unattended
recovery. A provider pause within the same diagnostic retains its epoch.

For an explicitly coordinated run, `--expect-sender-session NONZERO_UINT64`
pins the sender before first-packet admission and requires original-anchor mode.
Well-formed RTP bearing another provider/session is counted and discarded before
jitter-buffer branches or report slots are allocated. RTCP is accepted only for
SSRCs already seen in valid pinned RTP. This is not authentication: a colliding
SSRC in old RTCP can still match; RTCP alone cannot authorize foreign RTP PCM.
Malformed active RTP and existing continuity/clock/correction failures remain
fail-closed. The fixed eight-SSRC budget is unchanged.

`--control-stdin` requires that pin and enables a bounded controller lease;
`--replace-desktop-ipc` additionally requires explicit desktop output and opts into
validated predecessor retirement using IPC v2. An existing mapping is never
deleted to bypass its ownership checks. See [process-pair control](process-pair-control.md)
for readiness, stop/EOF semantics and the remaining physical/production gates.

Each received SSRC gets a bounded appsink. In original-anchor mode, the app
validates the extension and PCM inside the same owned, ordered RTP buffer,
before any depayloader can lose their association. Without `--expect-anchors`,
the previous nominal-reference-only depayloader path remains available. At most eight
sessions/report slots are accepted per finite process; further sessions fail
visibly. Stop the diagnostic before restarting a production component, rather
than making this session budget an unattended service.

Original-anchor validation requires contiguous wire-frame extents, the expected
clock epoch, an immutable format/origin, unchanged repeated records, and strictly
consecutive selected-anchor identities. Original capture age is limited to
250 ms old / 100 ms future. Repetitions do not refresh age or create estimator
windows. The active sender session may advance only to a higher generation with
a fresh, never-used SSRC and a valid frame-zero anchor. Retired generations and
foreign sessions cannot replace it. Faulted generations do not fall back to
arrival time or nominal RTCP references. See the
[experimental wire contract](audio-anchor-wire-draft.md) and
[validation results and limits](audio-anchor-transport-validation.md).

Packets without reference timestamp metadata remain counted as untimed priming.
No arrival-time or ordinary buffer-PTS fallback is used. Valid references must
have `timestamp/x-ntp` caps, be representable as signed nanoseconds, and be within
five seconds old / 100 ms future of the Linux clock. Each SSRC also needs a
sender report received within the last two seconds; cached/extrapolated reference
metadata alone is insufficient. These broad plausibility/freshness gates are
not a claim of sub-millisecond capture accuracy. This first receiver does not
validate replay resistance or send receiver reports/RTX feedback.

The finite report includes timed/untimed/stale/invalid counts, monotonicity,
greater-than-2-ms reference continuity differences, peak/RMS and reference age.
PCM is inspected in memory only and immediately released. Report freshness uses
the local monotonic receive time, not the time claimed by the incoming report.
Peak/RMS is not a listening or quality test, and silence is valid media.

Original mode also reports bounded per-SSRC ingress counts and first/last wire
positions, first ordered callback position, and the first validation failure
with checked capture age. Jitterbuffer drop messages are counted separately by
late/capacity reason. These identify a startup loss without erasing its first
cause behind the validator's subsequent latched-error reports. They contain no
PCM, hostnames or addresses, but raw timing diagnostics should still stay private.

Exit 0 means help or completed diagnostics. Exit 1 means configuration/pipeline
failure. With `--expect-media`, exit 3 means no timed media, stale/invalid
references, missing reference metadata after lock, or nonmonotonic timestamps.
That option does **not** fail on all
continuity gaps, startup untimed packets or silence: `timestamps_observed` is
deliberately weaker than `continuity_passed` or `audio_quality_passed`. Keep full
reports when comparing tests. Durations are 1..180 seconds; external driver/OS
shutdown stalls are not a hard-real-time watchdog guarantee.

`--expect-anchors` additionally returns exit 3 for invalid original records or
failure to observe at least three in-range estimator windows within the final
active generation, with its latest estimate still in range. Earlier generations
and repeated anchors cannot satisfy this gate. Reports expose all out-of-range
windows, rather than concealing them. Qualification is explicitly **historical**:
the receiver normally outlives its finite sender, and it does not claim present
lock, physical capture accuracy, or active correction. Those two modes do not
submit audio to the resampler or OBS. Missing/lost media currently faults the generation;
RTX and seamless re-priming are separate work.

Runtime dependencies include rtpbin, udpsrc, rtpL24depay and appsink. Build with
`AVSYNC_BUILD_NETWORK=ON`; see [building](building.md). Help/default CTest does
not bind sockets or capture audio. Do not publish local addresses, hostnames or
raw device logs in public bug reports.

## Opt-in correction diagnostic (still no OBS output)

Build with both `AVSYNC_BUILD_NETWORK=ON` and `AVSYNC_BUILD_ASRC=ON`, then add
`--correct-desktop`. This implies `--expect-anchors` and requires the audited
libsamplerate version. It does not affect default diagnostics or production
routes. Follow [measured quality/resource limits](audio-live-correction-validation.md)
before a live trial; on the tested host only the explicitly CPU-affined paced
trial met the proposed p99 CPU gate. Affinity is not installed automatically.

The ordered RTP callback validates the original record and decodes its same
packet's signed stereo L24 payload into an owned fixed-size float block. A short
mutex-protected copy places it in a 64-packet queue. The receiver's ordinary
main thread polls at 2 ms, drains at most 64 packets, and performs at most eight
480-frame DSP calls per dispatch. No DSP executes in the network callback and
no OBS callback is involved. Queue residence over 100 ms, overflow and backward
local time fail the diagnostic; the correction worker has its separate bounded
input/output/phase ledger and health watchdog.

The worker observes frame zero during RTCP startup but discards all priming
PCM. Running output additionally requires a sender report received within two
seconds. Missing/stale reports or a provider pause fail the current generation,
clear filter/queue state and require a separately admitted new generation.
There is no arrival-time rate estimate or timestamp rebasing. Corrected PCM is
inspected for finite values/levels and immediately discarded. No output is
audible, saved, scheduled for presentation, or submitted to IPC/OBS.

Reports separate ingress, ordered original frames, priming discards and corrected
frames. A normal correction diagnostic requires exactly one running session
with at least one second of inspected output. The explicit clock-pause fixture
requires exactly two, with the first health fault observed within 100 ms after
the actual provider-pause edge and the second running. Extra unplanned recovery,
wrong fault reasons/timing, queue failure or missing output return exit 3 rather
than treating eventual recovery as a pass. The first fault timestamp is retained.
Keep the sender active through the receiver deadline for this finite verdict:
ordinary sender termination correctly makes the diagnostic's current generation
stale. Teardown tails are not an exact sender/receiver packet-count test.

## Explicit desktop IPC handoff (not production migration)

With NETWORK, ASRC and IPC enabled, `--desktop-ipc NEW_PRIVATE_FILE` implies
`--correct-desktop` and writes corrected desktop PCM to the existing native
adapter's IPC protocol. The file must not exist; its parent must already be
private and user-owned. No OBS profile, source, service or microphone is changed.
This is a separate audio-only mapping; video and mic slots remain unused.

Capture-grid timestamps are preserved. Presentation is exactly capture + two
seconds, never arrival + two seconds. Complete 480-frame quanta are published
with nonblocking mutex acquisition into a preallocated 224-block stereo ring.
Brief mutex contention is retried from a fixed 16-block owned queue, with at most
eight publish attempts per dispatch and a 200 ms capture-age deadline. No sleeps
or unbounded backlog are used. Bad/late PCM, discontinuities, retry overflow/deadline,
clock loss or a new media generation
stop this finite handoff and revoke the mapping, rather than replaying queued
old audio. Creation/revocation are ordinary control-thread operations, not OBS
callbacks. **Automatic reconnection/restart is not implemented in this mode.**
The inspect/discard mode retains its separately tested recovery behavior.

Start a reader before audio begins to verify every due block, without playback:

```sh
./build-asrc/avsync-probe --path NEW_PRIVATE_FILE --duration 30 \
  --verify --desktop-only --delay-ms 2000
```

The reader checks exact delay, contiguous sequence, finite PCM and delivery, not
physical A/V sync. End it before the receiver's deadline; receiver shutdown
deliberately revokes undrained future audio. The private mapping contains recent
desktop PCM; do not publish it, and remove the test file/lock after all readers
and the producer exit. The [handoff checkpoint](startup-handoff-validation.md)
records what has actually been verified and the remaining OBS integration work.

## Explicit clock-loss fixture

The optional pair `--clock-pause-after N --clock-pause-seconds M` disables replies
from this process's own clock provider after N seconds, then restores them after
M seconds (1..10). Both are required, and the pause must finish before the normal
diagnostic deadline. RTP/RTCP reception remains active. It changes no OS clock,
firewall, other service or production sender. The report records whether the
pause and resume actually executed. This tests clock-health failover separately
from closing the media socket; it is not enabled by default. For example, a
45-second receiver can pause at 18 seconds for five seconds while a separately
started, bounded sender is active.
