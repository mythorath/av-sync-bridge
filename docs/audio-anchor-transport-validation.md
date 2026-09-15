<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Original capture-anchor transport validation

This milestone preserves original Windows audio-device timing through nominal
conversion and packetization. It is a **finite desktop transport diagnostic**,
not a completed synchronization bridge. It does not apply adaptive resampling,
play/record received audio, or submit real audio to OBS. The production audio
bridge, microphone mute, OBS profile, OS clocks, startup and firewall were not
changed during these checks.

## Implemented boundary

Each RTP packet carries a strict 96-byte original-anchor record, with its actual
first nominal wire-frame position separately represented. Original device
position, paired QPC time, mapped capture time and exact calibration revision
remain immutable across repetitions. Receiver validation and PCM inspection
operate on the same ordered RTP buffer, not independently paired queues.

The receiver prints a fresh clock-epoch token for explicit transfer to the
sender's command line. This manual finite-run agreement is not authentication
or a production recovery handshake. See the [wire contract](audio-anchor-wire-draft.md),
[sender](windows-sender.md), [receiver](network-receiver.md), and
[clock-mapping provenance](network-clock.md).

## Generated software checks

On Windows MSVC 19.44 / GStreamer 1.28.6 and Linux GCC 13.3 / GStreamer 1.24.2:

- The dependency-free codec/validator executable passed 11,198 checks, including
  an independent big-endian golden record, malformed/overflow controls, exact
  rational associations, immutable repeats, freshness and bounded generation
  admission. Repeat records cannot inflate estimator-window counts.
- The actual L24 payloader/extension and startup fixtures passed 3,397 checks.
  They cover irregular packet boundaries,
  split/aggregation, buffer lists, fractional-rate anchors and RTP timestamp
  wrap. An independent sample-index PCM oracle detects deliberately corrupted
  content even when its metadata remains structurally valid.
- The clock helper passed 10,222 checks, including exact single-snapshot mapping,
  calibration revision changes and exhausted/invalid mapping controls.

These RTP fixtures start with generated 48 kHz L24 PCM; they do not run the
nominal converter and transport together. Conversion has its own independent
[sample/phase fixtures](nominal-audio-validation.md). Generated tests prove
neither driver content-time accuracy nor real network-loss recovery.

Final CTest totals were 14/14 for the Windows optional-network build, 7/7 for
its dependency-free build, and 17/17 for Linux combined network/ASRC in both
optimized and AddressSanitizer/UndefinedBehaviorSanitizer builds. All 76 Python
measurement-helper tests also passed. No device or listener is opened by these
default tests.

The startup fixture independently chains real GStreamer RTP/RTCP session pads
in eight combinations of sender-report order and default/disabled probation.
All preserve the initial PCM on both tested runtimes. A separate controlled
jitterbuffer fixture holds only its local startup deadline in the future:
27 packets fit; 29 packets with the 100 ms capacity cap discard exactly frame
zero; disabling the cap retains all 29; starting after two consecutive packets
retains all 29. PCM identity and order are checked in every case. The fast-start
case explicitly waits for its first two outputs before the second bounded
burst, so it does not claim immunity to arbitrary worker stalls. These fixtures
passed ten consecutive runs per platform before the final full-suite checks.

## Live desktop observations — 2026-09-15

Tests used existing eight-channel 192 kHz float32 desktop loopback over a
dedicated trusted LAN. PCM was inspected only in memory. Nonzero peak/RMS is
signal evidence, not a listening or perceptual-quality test.

The initial 25-second overall sender run captured 2,332 packets and produced
1,119,232 stereo frames in 6,995 RTP packets. Linux received all of those frames
and packets. All 1,166 selected original anchors were represented, with 5,829
immutable repeats and 23 original-clock estimator windows. First/last reported
device positions and capture times, and the last selected QPC and anchor identity,
matched the sender exactly. There were no resets, post-start discontinuities,
timestamp errors, anchor faults, out-of-range rate windows, invalid references
or greater-than-2-ms nominal-reference gaps. Initial RTCP-reference priming
remained separately unqualified for 96 packets. Maximum observed conversion
call duration was 0.6772 ms; this is not a loaded-performance guarantee.

A subsequent 100-second run exposed a startup failure: the Linux input counted
all 29,558 RTP packets and 201 RTCP packets, but only 29,557 ordered RTP callbacks
were observed and none qualified. The fail-closed validator rejected the
generation instead of rebasing to a later packet. The original report retained
only the final latched error, so it did not establish the first rejection's
reason. This failure must not be represented as a successful long-run test.

### Startup failure isolated

A subsequent instrumented clock-pause run reproduced the startup failure in
its second generation. Its input first wire position was zero, but the first
ordered callback started at frame 180 and failed `bad_start`. The receiver
reported exactly one capacity drop and no too-late drops. The jitterbuffer log
showed 104.833 ms of media reaching the startup queue over 91.616 ms of arrival
time; its 100 ms release timer had not fired when the queue discarded frame zero.
The normal sender-report probation mechanism was not established as the cause.

Original-anchor mode now starts after two consecutive packets instead of waiting
the full latency before first release. The 100 ms capacity/loss budget remains
enabled. This is a receiver startup policy, not a change to original timestamps,
clock calibration, presentation delay or production OBS. Relevant upstream
contracts: [jitterbuffer startup/capacity](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html)
and [RTP session probation](https://gstreamer.freedesktop.org/documentation/rtpmanager/RTPSession.html).

### Controlled provider pause after the startup fix

A 40-second overall sender run paused only the diagnostic clock provider for
five seconds. The receiver saw two valid generations, both beginning at wire
frame zero, totaling 9,208 RTP packets / 1,473,344 stereo frames, exactly matching
the sender's output counters. There were no late/capacity jitter drops, original
anchor failures, out-of-range windows or invalid/backward nominal references.
The second generation produced 21 usable original-clock windows after a fresh
clock-health acquisition. Received PCM was nonzero in both generations.

This is interruption/reacquisition, not seamless audio. The sender discarded
758 capture packets while clock health was unqualified. It also discarded one
converted capture packet when its finite deadline expired: that packet's 480
frames appear in conversion totals but not transmitted/received totals. The
sender's two reset-counter increments include this final deadline cleanup;
there were only two transmitted generations, not a third recovery episode.
The receiver's qualification is historical, not a claim of present lock after
the finite sender has stopped.

A separate deliberate old-clock-epoch run admitted no PCM and reported
unqualified original anchors. Unit tests independently verify the precise
wrong-clock rejection, including latch behavior. Sender output alone never
counts as successful receiver admission.

### Longer run after the startup fix

The final 100-second overall sender run captured 9,856 packets and produced
4,730,752 stereo frames in 29,567 RTP packets. Sender output, Linux ingress and
ordered validated PCM totals matched exactly. All 4,928 selected original
anchors were represented, with 24,639 immutable repeats. First/last reported
device positions and mapped capture times, plus the last original QPC,
selected-anchor identity and calibration revision, matched across endpoints.

The receiver obtained 97 in-range original-clock windows. There were no sender
resets/drops/overflows, post-start discontinuities, timestamp errors, original
anchor failures, jitterbuffer drops, invalid/backward/stale nominal references,
or greater-than-2-ms nominal-reference gaps. Its first 68 reference-priming
packets remained explicitly unqualified; original-anchor admission began at
wire frame zero. PCM was nonzero. Maximum observed nominal-conversion call
duration was 0.896 ms, not a performance guarantee under gaming load.

This covers approximately 98.56 seconds of actual capture, not a 30-minute
soak or physical A/V test. Matching counters and original metadata do not
independently prove real PCM bit identity or perceptual quality. Test processes
and listeners were closed afterwards, and the normal production audio/OBS
services and microphone mute state were rechecked.

## Remaining gates

The desktop timing records are not yet a live ASRC command. Required next work
includes a bounded correction worker and presentation schedule; measured mapping
uncertainty and content-time calibration; real loss/reordering/restart recovery;
microphone capture/privacy; and combined physical video/audio OBS measurements
using nonuniform markers. Thirty-minute representative-load and complete
restart-matrix gates remain open.
