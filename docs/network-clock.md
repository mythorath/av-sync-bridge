# Shared network clock contract

This is an experimental capture-time mapping helper, not a claim that the real
devices, transport, or OBS are synchronized. Its deterministic tests do not use
audio devices or create network sockets. It uses GStreamer APIs; no clock-sync
protocol or retransmission implementation is copied into this project.

## One monotonic time domain

The Linux receiver's selected, unadjusted monotonic `GstSystemClock` is exported
through `GstNetTimeProvider`. The provider must be bound to the explicitly selected
private link, not an unrestricted interface. Before starting it, compare that
clock's **internal** time with the daemon/IPC `CLOCK_MONOTONIC` domain. If the
comparison fails, stop; do not introduce an arrival-time offset to hide it.
The Windows sender follows the provider with `GstNetClientClock`. This does not
change either operating system's wall clock or NTP configuration.

For each WASAPI packet, `GetBuffer` supplies a device-frame position and the QPC
position of the **first audio frame**. WASAPI has already converted the QPC value
into 100-nanosecond units. The helper multiplies by 100, with overflow checks; it
does not divide by the hardware QPC frequency again. GStreamer 1.28.6's Windows
monotonic system clock scales the same raw QPC counter into nanoseconds without
subtracting a process-specific origin. A bounded local before/after comparison
checks this assumption at runtime. The check establishes a compatible clock
domain, not the accuracy of an audio driver's capture timestamp.
See [WASAPI GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)
and the pinned [GStreamer system-clock implementation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gstreamer/gst/gstsystemclock.c).

Loopback receipt is not proof that the corresponding sound has physically
played. Microsoft documents that the audio engine can copy output to the
loopback buffer alongside its copy to the hardware render pin. A device-supplied
frame timestamp may therefore need an explicitly bounded future-time acceptance
policy; the API documentation does not certify a particular observed lead as
accurate. The sender's diagnostic tolerance must preserve the timestamp, record
the signed lead, and reject values outside the bound. Do not clamp it to arrival
time or subtract a guessed output latency. The observed lead is not itself a
measurement of hardware content latency. `GetStreamLatency` reports a stream
maximum, not an automatic per-packet timestamp correction. See
[Microsoft loopback behavior](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
and [stream latency](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getstreamlatency).

The mapping is a calibration snapshot applied to the original capture timestamp:

```
shared_capture_ns = remote_reference
                  + (local_capture_ns - local_reference) * rate_num / rate_den
```

Integer scaling follows GStreamer's floor-scaled magnitude convention on either
side of the reference. Invalid, negative-domain or signed-overflow values are
rejected. There is no "time when the packet arrived" term and no rebase to the
time when the mapping function is called.

`GstNetClientClock` is a wrapper: once synchronized, its `get_internal_time()`
already returns the adjusted time of its underlying clock. Therefore applying
the wrapper's usually-identity calibration to a raw QPC value is incorrect.
`client_calibration()` reads the readable `internal-clock` property and snapshots
**that clock's** calibration. It rejects an unsynchronized clock, an unexpected
clock type or a nonidentity, user-applied transform on the wrapper. These details
were checked in both [1.24.2](https://github.com/GStreamer/gstreamer/blob/1.24.2/subprojects/gstreamer/libs/gst/net/gstnetclientclock.c)
and [1.28.6](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gstreamer/libs/gst/net/gstnetclientclock.c).
See also [GstClock calibration APIs](https://gstreamer.freedesktop.org/documentation/gstreamer/gstclock.html),
[NetClientClock](https://gstreamer.freedesktop.org/documentation/net/gstnetclientclock.html)
and [NetTimeProvider](https://gstreamer.freedesktop.org/documentation/net/gstnettimeprovider.html).

## Acquisition and operational health

The caller owns the client clock and a dedicated statistics bus. Drain that bus
regularly and give `gst-netclock-statistics` element messages to
`ClockHealthMonitor::observe()`. Both the statistics receive time and
`gst_util_get_timestamp()` are in the sender's raw local monotonic domain.
Malformed, overflowing, duplicate and out-of-order observations are rejected.

The default `health()` gates require all of the following:

- The public clock sync flag and a valid calibration snapshot.
- At least four valid statistics observations, with the newest at most 2 seconds old.
- The newest reported round trip at most 5 milliseconds.
- A calibration rate within 2,000 ppm of the local clock and a last reported
  calibration discontinuity no larger than 2 milliseconds in magnitude.

These are configurable **operational rejection thresholds**, not a demonstrated
clock-error bound. A statistics observation is not necessarily an applied
calibration update. Its `synchronised` field is a per-observation algorithm
diagnostic; it is not substituted for the public clock state. GStreamer's source
can keep the public sync flag set after provider replies stop, so freshness must
be checked independently. Round-trip time, sample count and regression fit do
not prove one-way symmetry or absolute offset accuracy. In particular this API
does not expose a misleading "RTT/2 is guaranteed uncertainty" result.

Acquisition has a caller-enforced deadline. On loss of health, stop accepting
capture into the current transport generation; do not keep sending on an
unbounded holdover. After reacquisition, reset the health state and establish a
fresh stream generation/SSRC. Mapping and health are separate snapshots, so the
sender must also check mapped capture timestamps for backwards movement,
unexpected discontinuities and incompatible device-frame progression. A WASAPI
timestamp-error packet cannot be rescued by stamping it with the arrival time.
This helper does not perform reconnects, own a session token, or implement ASRC.

## RTP correspondence for the first transport probe

The initial wire format is RTP payload type 96, `L24`, 48 kHz, two channels. Sender
buffer PTS represents shared absolute monotonic capture time. Pipeline base time
is zero and start time is `GST_CLOCK_TIME_NONE`. `rtpbin` uses
`ntp-time-source=clock-time` with `rtcp-sync-send-time=false`; the RTCP sender
report relates the media RTP timestamp to that shared clock, not UDP send time.
This is **not UTC**, despite the RTCP field's NTP-format representation. See the
sender-time correspondence in [GStreamer RTP session source](https://github.com/GStreamer/gstreamer/blob/1.24.2/subprojects/gst-plugins-good/gst/rtpmanager/gstrtpsession.c)
and [rtpbin properties](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpbin.html).

The receiver must stay priming until it obtains and validates SR-derived
reference timestamp metadata; it must not silently substitute packet arrival
time. `rtpjitterbuffer`'s `add-reference-timestamp-meta` facility is available
since 1.22. Reconstructed metadata can continue by extrapolation after sender
reports stop; a production receiver must independently gate the age of its last
validated sender report, not treat metadata presence as proof of fresh RTCP.
Metadata propagation through depayloading and packet fragmentation
must be tested against the installed runtimes. A future every-packet NTP64
extension may shorten acquisition, but is not an assumed fallback. It would
need an explicit non-UTC reference convention and a verified timestamp for each
outgoing packet, not an unchanged metadata copy from a larger input block.
See [jitterbuffer reference metadata](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html)
and [NTP64 extension](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtphdrextntp64.html).

The sender does not add the presentation buffer. The receiver schedules the
validated capture time against the one configured total presentation delay.
Retransmission must use GStreamer's RTP/RTCP mechanisms within that deadline;
clock metadata alone neither prevents loss nor makes late replay safe.

## Conversion, drift and remaining proof gates

The channel mixer and fixed-ratio resampler must preserve the capture-time
meaning of their output PTS. GStreamer's audio resampler derives output PTS from
its starting input PTS and output sample count; its filter latency is separately
reported in a latency query, not blindly added to the PTS. Do not manually add
that latency twice or attach one input reference timestamp unchanged to every
output fragment. Fixed-ratio conversion is not an automatic correction for
long-term audio-device oscillator drift. See the pinned
[audio resampler implementation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c).

Still required before production: measured clock residuals under load and
provider loss; sender/receiver restart generations; original-capture timestamp
recovery from actual RTP/RTCP; deadline-bounded loss recovery; a single validated
audio rate-matching owner; and hardware A/V measurement across restarts and long
runs. Device/converter content latency still requires nonuniform-marker
calibration. Synchronizing computer clocks cannot discover that content delay
or genlock the independent audio and video oscillators.
