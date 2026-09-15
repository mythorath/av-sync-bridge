<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Original audio anchor transport: wire draft

Status: **implemented experimental desktop-audio diagnostic; not a stable wire
contract or production sync bridge**. The bounded codec, RTP adapter, ordered
validator and generation-admission helper are implemented. The receiver can
inspect original anchors alongside their RTP PCM with `--expect-anchors`.
It does not apply ASRC, schedule presentation, record/play audio, or send this
path to OBS. Generated fixtures establish software properties, not physical
capture accuracy or end-to-end A/V synchronization.

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

## Experimental fixed 96-byte record

The narrow diagnostic profile uses one RFC 8285 **two-byte-header** element,
with appbits zero and extension ID 1. Both endpoints explicitly agree to this
profile; ID 1 is a prototype choice, not a global assignment or negotiated URI.
All multibyte values are network byte order, serialized explicitly rather than
copying a native C++ struct. The adapter rejects unknown or duplicate elements,
CSRCs, RTP padding and nonzero extension padding.

| Byte offset | Type | Field |
|---:|---|---|
| 0 | u8 | Version, exactly 1 |
| 1 | u8 | Flags, exactly 0 in this desktop-only draft |
| 2 | u16 | Record size, exactly 96 |
| 4 | u32 | Nominal original source sample rate |
| 8 | u64 | Nonzero sender session ID |
| 16 | u64 | Nonzero conversion generation |
| 24 | u64 | Nonzero provider-clock epoch explicitly copied from this receiver run |
| 32 | u64 | Original device-frame origin for this generation |
| 40 | u32 | RTP timestamp corresponding to wire frame zero |
| 44 | u32 | Reserved, exactly 0 |
| 48 | u64 | First wire frame in this packet's L24 payload |
| 56 | u64 | Selected original-anchor sequence |
| 64 | u64 | Original anchor device-frame position |
| 72 | i64 | Original mapped capture time, nonnegative nanoseconds |
| 80 | u64 | Original WASAPI QPC time in 100 ns units |
| 88 | u64 | Nonzero revision of the exact calibration used for this mapping |

The packet header supplies SSRC; admitted `(session, generation)` binds to one
SSRC and one immutable source rate, origin, clock epoch and RTP-zero timestamp.
All fields of an identified anchor, including QPC time and calibration revision,
remain immutable when repeated. Only the per-packet wire start changes.
The codec accepts source rates from 8 to 384 kHz, nonnegative mapped capture
times and raw QPC values whose conversion to signed nanoseconds is representable.
The nominal converter independently applies its narrower phase-table bounds.
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

## Explicit agreement for a finite diagnostic

The receiver generates a fresh nonzero clock-epoch token and prints it in
`AVSYNC_NETWORK_READY ... clock_epoch=...`. Copy that exact token to the sender's
required `--clock-epoch` argument for this bounded run. A restarted receiver
requires its new token and a new run; do not reuse a saved value. This is a
manual, out-of-band agreement, **not an automatic control handshake,
authentication, or proof of clock accuracy**.

`CaptureClockMapper` obtains one calibration snapshot and returns the original
QPC conversion, mapped capture time, exact applied coefficients and revision as
one value-owned result. Selected anchors retain that result unchanged. Normal
calibration changes advance its revision, not the transport generation; invalid
mapping or health loss still fences the generation. See the
[shared-clock contract](network-clock.md).

## Implemented sender ledger and receiver validation

- Select actual original capture anchors at least 20 ms apart on the nominal
  device-frame grid, beginning with the conversion origin. Assign consecutive
  IDs to selected anchors, not to every capture buffer. Never manufacture a
  capture time at a convenient interval.
- Retain selected records in a **32-record FIFO** until packetization passes
  their nominal positions; overflow is a fault rather than an overwrite.
  At packet decoration, the selected anchor's original capture time must be
  at most **200 ms old** and at most 100 ms in the future. This is a capture-age
  check on the selected record, **not a strict 200 ms FIFO-residence deadline**
  or an independent timer expiring every queued record.
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
  The diagnostic inspects the extension and PCM in the same owned RTP buffer,
  before releasing it; it does not forward PCM to an ASRC worker or maintain
  independently paired metadata/audio FIFOs. Future media delivery must retain
  that atomic ownership. The jitter buffer reorders and removes duplicates;
  its output PTS is not an original capture anchor.
  [Jitter-buffer contract](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html).
- Reject absent/duplicate target elements, unknown versions/flags, wrong size,
  nonzero reserved fields, zero or non-frame-aligned PCM, arithmetic overflow,
  wrong epochs and unapproved source-format changes. Require
  `rtp_timestamp == (rtp_zero + packet_wire_start) mod 2^32`; contiguous accepted
  packet extents must advance by their actual payload frames. Do not use a
  periodic timestamp or SSRC alone to identify a restarted stream.
- First admission requires wire frame zero, selected-anchor sequence zero and
  the original device position equal to the declared device origin. Every
  selected anchor's exact rational position must be no later than the first
  wire frame of its carrier packet. Receiver capture age is bounded to **250 ms
  old / 100 ms future**. Consecutive new anchors require strictly increasing
  original QPC/device positions and capture times, with nondecreasing
  calibration revision.
- Identical repeated anchors are deduplicated before `AudioAnchorTracker`;
  repetitions do not advance its sequence or refresh original capture age.
  A repeated ID with changed contents is a fault. Missing, backward, stale or
  conflicting selected anchors fail closed under the current tracker contract.
  Do not renumber received records, turn missing media into declared silence,
  or fall back to nominal SR timestamps to keep correction apparently active.
- New rate measurements are emitted only for newly completed original-clock
  windows. Repeated anchors do not inflate measurement counts. Out-of-range
  values remain diagnostic evidence; the separately queried correction estimate
  is withheld when stale, faulted or outside the correction limit.
- Each ordered branch has an O(1) validator which latches same-generation
  faults; later valid-looking packets do not heal a gap. Global admission binds
  the first sender session and SSRC, then permits only higher generations of
  that same session on new, never-reused SSRCs starting at wire frame zero.
  It remembers at most **eight admitted SSRCs**, rejecting retired generations,
  foreign sessions and further transitions after the cap. Rejected admission
  does not replace the active generation. This is bounded finite-run policy,
  not general restart recovery or authenticated sender identity.

## Generated transport fixtures

The dependency-free `avsync-audio-wire-tests` covers the independent 96-byte
golden representation, malformed fields, overflow, rational associations,
original-clock measurements, immutable repeats, age limits and admission rules.
`avsync-rtp-audio-anchor-tests` exercises the actual GStreamer L24 payloader and
adapter with sample-index-coded PCM, split/aggregated input, buffer lists,
44.1 kHz associations, RTP timestamp wrap and deliberately invalid packets.
Ordered-validator negative cases include missing/reordered media, lost anchor
transitions, stale anchors and incorrect epochs. These are generated software
fixtures; they do not establish real network-loss recovery, capture-driver
accuracy, live correction quality or physical A/V timing.

## Remaining production gates

1. **Managed control and recovery.** Replace manual finite-run token transfer
   with explicit provider/sender-instance agreement and safe process-restart
   coordination. Define retirement, reacquisition and operator-visible failures
   across real link loss; do not silently admit a new sender session by arrival.
2. **Physical timing and mapping uncertainty.** Exact applied-coefficient
   provenance is implemented, but it is not a hardware-atomic capture/clock
   observation or clock-error bound. Measure clock residuals, driver content
   timing and calibration behavior under load and across restarts.
3. **Owned PCM through correction and presentation.** Connect validated packet
   identity to bounded ASRC input/output and one presentation timeline. On a
   fault, discard prior-generation PCM, anchors and filter history before
   re-priming. Jitter latency and a 32-record ledger alone do not prove total
   process-memory or end-to-end deadline bounds.
4. **Live and end-to-end proof.** Extend generated tests to real transport loss,
   all repetitions lost, control restart races, long runs and physically measured
   nonuniform audio/video events. The generated converter, metadata and PCM
   fixtures do not substitute for that evidence.

These checks provide protocol consistency, **not authentication or encryption**.
The current private-link UDP/RTP transport and source-IP filtering cannot prove
who sent a packet or protect metadata from tampering. Raw QPC/device positions
also expose timing information. Do not extend this draft to untrusted networks
or claim secure identity without a separately designed authenticated transport.
Desktop-only remains the scope; a microphone needs independent anchors,
correction state and privacy-generation behavior.
