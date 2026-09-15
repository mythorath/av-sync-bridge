# Video timestamp and sequence policy

This portable policy validates driver metadata; it does not establish physical
A/V synchronization. The Linux capture adapter translates V4L2 flags, rejects
corrupt/unsupported frames, checks timestamp age against the local monotonic
clock, and copies accepted image data before returning the driver buffer.

## Timestamp provenance

V4L2 separates the clock type from the point within a frame. `MONOTONIC` identifies
`CLOCK_MONOTONIC`; `UNKNOWN` can represent monotonic or realtime, and `COPY` belongs
to a separate output-to-capture timestamp contract. `SOE` advertises exposure
start; `EOF` advertises the last pixel's receipt/transmission. The source mask's
zero value means EOF, not an absent flag. Driver flags are expected to remain
constant within a stream. These are API labels, not independent measurements of
when an HDMI image was generated. See the [V4L2 buffer specification](https://docs.kernel.org/userspace-api/media/v4l/buffer.html).

`TimestampClock` and `TimestampPoint` preserve those separate facts. Reserved
clock/source values are normalized to `unknown` by the platform adapter. The
portable `timestamp_from_timeval` accepts only the monotonic clock, nonnegative
seconds, microseconds in `[0, 999999]`, and results fitting signed 64-bit
nanoseconds. Zero is representable here; a capture-age check independently
decides whether it is plausible. Invalid metadata has no arrival-time fallback.
The source point does not change the numerical timestamp or apply a guessed
frame-period adjustment. Unknown source provenance must remain visible.

### UVC caution: the flag is not the complete implementation

Inspected upstream Linux **v7.0** sources on 2026-09-15:

- [`uvc_queue.c`](https://github.com/torvalds/linux/blob/v7.0/drivers/media/usb/uvc/uvc_queue.c#L224-L245)
  advertises monotonic/SOE flags when creating the video queue.
- [`uvc_video.c`](https://github.com/torvalds/linux/blob/v7.0/drivers/media/usb/uvc/uvc_video.c#L1125-L1148)
  initially timestamps buffer activation while processing USB payloads.
  Its [clock-update path](https://github.com/torvalds/linux/blob/v7.0/drivers/media/usb/uvc/uvc_video.c#L739-L842)
  can replace that timestamp using device/USB/host-clock interpolation, but exits
  immediately when hardware timestamping is disabled. Its
  [time helper](https://github.com/torvalds/linux/blob/v7.0/drivers/media/usb/uvc/uvc_video.c#L463-L469)
  can select monotonic or realtime.
- [`uvc_driver.c`](https://github.com/torvalds/linux/blob/v7.0/drivers/media/usb/uvc/uvc_driver.c#L2304-L2329)
  exposes the clock and hardware-timestamp module parameters.

Therefore, a monotonic/SOE flag alone does not prove hardware-derived exposure
time. This inference motivates recording kernel/driver versions, module settings,
observed flags, and timestamp-age distributions during physical validation.
Distribution patches may differ from this upstream tag. The bridge does not
change global module parameters, and does not automatically enable hardware
timestamping. Fixed path calibration and repeated nonuniform-marker measurements
remain necessary; buffering cannot recover unknown variable delay before the
reported timestamp.

## Chronology without rebasing

`VideoTimingTracker` is scoped to one progressive capture generation. The driver
sequence field counts frames and may reveal host-side losses, but does not
necessarily expose upstream device losses. Alternating fields can share a
sequence value, so this progressive-only policy must not be applied to them.
See the [V4L2 sequence definition](https://docs.kernel.org/userspace-api/media/v4l/buffer.html#c.v4l2_buffer).

The tracker computes unsigned 32-bit modular distance from its last accepted
sequence. Distance one is normal, zero is repeated, `2..2^31-1` is a forward gap,
exactly `2^31` is ambiguous, and larger distances are treated as backwards. Thus
`0xffffffff -> 0` is normal, not a reset. This half-range convention is our
conservative continuity policy, not a claim that arbitrary multi-year gaps can
be disambiguated from replay.

Timestamp status is evaluated independently: first, forward, repeated,
regression, or invalid. Only first/forward timestamps paired with first/normal/gap
sequences are accepted. Duplicate timestamps are rejected even if sequence
advances. A forward gap returns its missing-sequence count, but only an accepted
observation contributes accepted loss. Timestamp spacing is preserved; missing
frames are never removed from the timeline to make output appear continuous.

Rejected observations do not move the tracker's anchor. The capture owner may
stop rather than continue after invalid chronology; restarting requires an
explicit new capture generation. Device-clock resets must not silently become a
new origin. Original capture timestamps remain distinct from later calibrated
presentation deadlines.

## Deterministic coverage

`avsync-video-timing-tests` uses runtime checks that remain enabled in Release
builds. It covers canonical timeval input, integer boundaries, unknown/copied
clock rejection, wraparound, forward gaps, repeated/backwards/ambiguous sequences,
equal/regressing timestamps, rejected-anchor preservation, and a generated
10,000-frame progression crossing sequence wrap. No device is opened, no media
is read, and these checks do not validate a capture card or OBS output.
