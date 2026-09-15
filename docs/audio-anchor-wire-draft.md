<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Original audio anchor transport: wire draft

Status: **design proposal only; unimplemented; not a committed or stable wire
contract**. The sender currently keeps original anchors locally. Neither the
extension below nor receiver ASRC is enabled by this document. Field choices,
admission and recovery must pass fixtures before becoming a versioned protocol.

## Two independent positions

Each RTP packet needs its actual first **nominal 48 kHz wire-frame index**. A
separate original anchor contains an unchanged WASAPI device-frame position and
its first-sample QPC capture time mapped to the shared monotonic clock. The
anchor is not a timestamp for the packet carrying it. Do not substitute packet
arrival, converter input-consumption counts, or nominal RTP/RTCP timestamps for
original capture timing.

For one conversion generation, declare `wire_origin = 0` and retain its original
`device_origin`. An anchor's nominal association is exactly:

`(anchor_device_position - device_origin) * 48000 / source_rate`

Keep the quotient and remainder as in [audio-anchors.md](audio-anchors.md), rather
than rounding each capture packet. Delayed converter output and RTP splitting
or aggregation do not change this relationship. One capture buffer is not one
RTP packet, and packet sequence numbers are not sample indices.

The generated [nominal conversion fixtures](nominal-audio-validation.md) now
establish the tested converter's unshifted nominal grid, counts, fractional-rate
phase and fixed/irregular partition agreement, including downsampling. That is
a completed offline prerequisite, **not** proof of network metadata association,
physical capture timing, sustained clock correction or end-to-end OBS sync.

## Proposed fixed 96-byte record

Use one RFC 8285 **two-byte-header** element, with appbits zero. Bind a project
URI and extension ID through explicit out-of-band configuration; ID 1 is a
prototype choice, not a global assignment. All multibyte values are network
byte order; serialize fields explicitly, never copy a native C++ struct.

| Byte offset | Type | Field |
|---:|---|---|
| 0 | u8 | Version, exactly 1 |
| 1 | u8 | Flags, exactly 0 in this desktop-only draft |
| 2 | u16 | Record size, exactly 96 |
| 4 | u32 | Nominal original source sample rate |
| 8 | u64 | Nonzero sender session ID |
| 16 | u64 | Nonzero conversion generation |
| 24 | u64 | Agreed provider-clock epoch |
| 32 | u64 | Original device-frame origin for this generation |
| 40 | u32 | RTP timestamp corresponding to wire frame zero |
| 44 | u32 | Reserved, exactly 0 |
| 48 | u64 | First wire frame in this packet's L24 payload |
| 56 | u64 | Selected original-anchor sequence |
| 64 | u64 | Original anchor device-frame position |
| 72 | i64 | Original mapped capture time, nonnegative nanoseconds |
| 80 | u64 | Original WASAPI QPC time in 100 ns units |
| 88 | u64 | Revision of the exact calibration used for this mapping |

The packet header supplies SSRC; admitted `(session, generation)` binds to one
SSRC and one immutable source rate, origin, clock epoch and RTP-zero timestamp.
All fields of an identified anchor, including QPC time and calibration revision,
remain immutable when repeated. Only the per-packet wire start changes.
The profile remains ordinary stereo L24/48 kHz RTP if a nonparticipating reader
ignores the extension; the sync application must not enable correction without
validated metadata. [RFC 8285](https://www.rfc-editor.org/rfc/rfc8285.html).

The record adds **104 bytes**: four bytes of RTP extension framing, two bytes of
element framing, 96 data bytes, and two padding bytes. For a final RTP packet
limit of 1,200 bytes, reserve this growth before payload construction: a
1,096-byte pre-extension limit leaves up to 1,080 PCM bytes, or 180 stereo L24
frames, with a 12-byte RTP header and no CSRCs. The resulting maximum populated
packet is 1,196 bytes. Check final size after insertion; do not grow an already
1,200-byte packet or rely on IP fragmentation. These are RTP sizes, excluding
UDP/IP headers. GStreamer's two-byte extension insertion can fail and must be
checked. [RTP buffer API](https://gstreamer.freedesktop.org/documentation/rtplib/gstrtpbuffer.html).

## Sender ledger and receiver validation

- Select actual original capture anchors at least 20 ms apart on the nominal
  device-frame grid, beginning with the conversion origin. Assign consecutive
  IDs to selected anchors, not to every capture buffer. Never manufacture a
  capture time at a convenient interval.
- Retain selected records in a bounded FIFO until packetization passes their
  nominal positions. A proposed initial cap is 32 records and 200 ms residence;
  overflow/expiry invalidates the generation, never overwrites required records.
  Reconcile these caps with converter, appsrc and network budgets before use.
- For each packet, repeat the newest selected anchor whose **exact rational**
  position is no later than its first wire frame. Keep that anchor until its
  successor applies. This avoids a racing "latest capture" pointer attaching
  unrelated metadata while the converter or packetizer is behind. With packet
  spans capped at 180 frames, a 20 ms selected-anchor interval has multiple
  opportunities for transmission. This is repetition, not guaranteed delivery.
- Verify the sender's packet position against its contiguous converted sample
  ledger, RTP timestamp progression and payload frame count. Do not merely
  count outgoing bytes if an upstream drop could have been hidden. Buffer-list
  output, split buffers and aggregation all require the same checks.
- Validate peer and packet bounds at ingress. After jitter-buffer ordering,
  parse RTP, the extension and L24 payload together, **before depayloading**.
  Keep validated PCM and its packet identity in one bounded owned object; do
  not assume arbitrary metadata survives a depayloader or pair independent
  metadata/audio FIFOs by arrival order. The jitter buffer reorders and removes
  duplicates; its output PTS is not an original capture anchor.
  [Jitter-buffer contract](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html).
- Reject absent/duplicate target elements, unknown versions/flags, wrong size,
  nonzero reserved fields, zero or non-frame-aligned PCM, arithmetic overflow,
  wrong epochs and unapproved source-format changes. Require
  `rtp_timestamp == (rtp_zero + packet_wire_start) mod 2^32`; contiguous accepted
  packet extents must advance by their actual payload frames. Do not use a
  periodic timestamp or SSRC alone to identify a restarted stream.
- Identical repeated anchors are deduplicated before `AudioAnchorTracker`;
  repetitions do not advance its sequence or refresh original capture age.
  A repeated ID with changed contents is a fault. Missing, backward, stale or
  conflicting selected anchors fail closed under the current tracker contract.
  Do not renumber received records, turn missing media into declared silence,
  or fall back to nominal SR timestamps to keep correction apparently active.
- A media gap, expired anchor, clock fault or admitted new generation discards
  old PCM, anchors, ASRC history and sample-grid state before re-priming. Old or
  unadmitted SSRCs cannot switch the active generation back. The required
  sender/receiver restart coordination remains to be designed; the first
  finite diagnostic can stop explicitly rather than pretend recovery exists.

## Prerequisites still missing

1. **Provider-clock epoch agreement.** The current network-time provider does
   not distribute the proposed epoch token. An explicit startup/control handshake
   must bind the expected provider instance and sender session. A guessed token,
   SSRC, process arrival time or wall-clock timestamp is not such a handshake.
2. **Atomic calibration provenance.** Mapping currently returns a timestamp;
   a separately read calibration snapshot can race it. Add one operation that
   snapshots the exact coefficients/revision and applies that snapshot to the
   original QPC value. Normal calibration revisions are provenance, not automatic
   generation changes. Health loss or a disallowed mapping discontinuity must
   fence a generation; the policy and uncertainty remain independently tested.
3. **Admission, bounded ownership and faults.** Define active/retired epoch
   admission, frame/anchor queues, expiry, control-channel recovery and all
   ownership across capture, packetizer, jitter buffer and ASRC worker. A
   configured jitter latency alone is not proof of a total process-memory bound.
4. **Independent transport fixtures.** Exercise capture/packet boundary mismatch,
   delayed conversion, buffer lists, 44.1 kHz rational positions, 32-bit RTP wrap,
   duplicate/reordered/lost packets, all repetitions lost, stale anchors, wrong
   SSRC/clock epochs and restart races. Require sample-content markers as well
   as metadata/count checks, with deliberately wrong associations rejected.

These checks provide protocol consistency, **not authentication or encryption**.
The current private-link UDP/RTP transport and source-IP filtering cannot prove
who sent a packet or protect metadata from tampering. Raw QPC/device positions
also expose timing information. Do not extend this draft to untrusted networks
or claim secure identity without a separately designed authenticated transport.
Desktop-only remains the scope; a microphone needs independent anchors,
correction state and privacy-generation behavior.
