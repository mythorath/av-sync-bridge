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

## Applied-coefficient provenance and explicit run agreement

`CaptureClockMapper::map()` obtains `client_calibration()` exactly once, maps
the historical QPC value with that snapshot, and returns a value-owned
`MappedCapture`: local nanoseconds, mapped capture nanoseconds, the exact
coefficients used, and their revision. A later calibration update cannot alter
an earlier result. The first successful mapping has revision 1; any changed
coefficient tuple advances it, including equivalent transforms represented
using different reference values. Invalid mappings and exhausted revision
budgets do not advance state. Keep the mapper across conversion generations.

This solves consistency between the applied mapping and its recorded provenance;
it is **not a hardware-atomic capture/clock observation**, a health check, or a
statistical accuracy guarantee. Selected original anchors carry the unchanged
QPC value, mapped time and revision. The receiver requires immutable repeated
anchors, increasing QPC and nondecreasing revisions for new anchors. Normal
revision changes alone do not trigger a new stream generation.

For the finite diagnostic, the receiver generates a fresh nonzero provider-clock
epoch and prints it in `AVSYNC_NETWORK_READY ... clock_epoch=...`. The operator
copies that token to the sender's `--clock-epoch` option. `--expect-anchors`
enables the receiver's original-anchor validation. A receiver restart requires
a new token and explicit new run. This is **manual out-of-band agreement**, not
an automatic startup handshake, authentication, a secret credential, or evidence
that the provider clock is accurate. See the implemented experimental
[anchor wire contract](audio-anchor-wire-draft.md) for bounded admission rules.

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
fresh stream generation/SSRC. Mapping provenance is consistent within one
`MappedCapture`, but mapping and health are still separate snapshots, so the
sender must also check mapped capture timestamps for backwards movement,
unexpected discontinuities and incompatible device-frame progression. A WASAPI
timestamp-error packet cannot be rescued by stamping it with the arrival time.
This helper does not perform reconnects, own a session token, or implement ASRC.

For clock-loss diagnosis, remember that GStreamer's statistics bus is not a
packet-receipt heartbeat. In the pinned 1.28.6 implementation, the configured
RTT limit, twice-median filter and twice-average filter run before statistics
are posted. Rejected observations schedule a separate fixed 250 ms retry;
lowering the internal adaptive timeout does not alter that rejection path.
An absence of accepted statistics therefore does not alone prove the provider
or network stopped. Preserve a private, finite `GST_DEBUG=netclock:6` trace to
distinguish missing replies from the exact rejection branch before changing
polling or safety limits. Debug traces can include peer addresses and belong
outside the public repository. This diagnostic does not relax health gates.
[Pinned upstream filtering and statistics](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gstreamer/libs/gst/net/gstnetclientclock.c#L387-L598).

## Nominal RTP time and original capture anchors

The wire format is RTP payload type 96, `L24`, 48 kHz, two channels. After nominal
conversion, sender buffer PTS follows the exact **nominal 48 kHz sample grid**
anchored at the generation's first mapped capture time. It is not a continuing
measurement of the device's actual oscillator rate or the capture time of each
later fragment. Pipeline base time is zero and start time is
`GST_CLOCK_TIME_NONE`. `rtpbin` uses
`ntp-time-source=clock-time` with `rtcp-sync-send-time=false`; the RTCP sender
report relates the media RTP timestamp to the nominal timeline expressed in
that shared clock domain, not UDP send time.
This is **not UTC**, despite the RTCP field's NTP-format representation. See the
sender-time correspondence in [GStreamer RTP session source](https://github.com/GStreamer/gstreamer/blob/1.24.2/subprojects/gst-plugins-good/gst/rtpmanager/gstrtpsession.c)
and [rtpbin properties](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpbin.html).

Legacy SR-derived reference metadata remains a separate transport diagnostic;
it must not be passed to the device-rate estimator as original capture timing.
`rtpjitterbuffer`'s `add-reference-timestamp-meta` facility is available
since 1.22. Reconstructed metadata can continue by extrapolation after sender
reports stop; a production receiver must independently gate the age of its last
validated sender report, not treat metadata presence as proof of fresh RTCP.
The implemented original-anchor mode instead parses a fixed 96-byte extension
and its PCM together **after jitter-buffer ordering, before depayloading**.
It validates packet frame positions independently of a repeated original
WASAPI sample-position/capture-time anchor. Selected anchors are at least 20 ms
apart on the nominal device grid; their exact rational association handles
converter delay, splitting and aggregation without arrival-time pairing.
The sender retains at most 32 selected records and checks the selected capture
time at decoration against 200 ms old / 100 ms future. That check is not a FIFO
residence timer. The receiver applies 250 ms old / 100 ms future, and repeated
anchors never refresh original capture age. Neither SR metadata nor an NTP64
extension is a fallback when original-anchor validation fails.
See [jitterbuffer reference metadata](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpjitterbuffer.html)
and the [anchor wire contract](audio-anchor-wire-draft.md).

Neither finite diagnostic adds the production presentation buffer or applies
adaptive rate correction. A future receiver must schedule validated media
against one configured total presentation delay and clear old-generation state
on failure. Deadline-bounded retransmission/recovery remains to be designed;
clock metadata alone neither prevents loss nor makes late replay safe.

## Conversion, drift and remaining proof gates

The sender now uses a timestamp-free nominal converter rather than feeding
drifting capture PTS into the `audioresample` element. Generated fixtures test
its unshifted sample grid, rational-rate frame counts and partition agreement;
see [nominal conversion validation](nominal-audio-validation.md). Filter
lookahead delays availability, not the declared content-origin grid. Do not
add that delay twice, replace original capture anchors with nominal output PTS,
or attach one capture time unchanged as the timestamp of every output fragment.
Nominal conversion is not correction of long-term audio-device oscillator drift.

The bounded codec/validator and actual-payloader generated fixtures now cover
explicit endian encoding, packet splitting/aggregation, original-anchor
association, sample-index-coded PCM, RTP wrap and fail-closed invalid cases.
The diagnostic admits at most eight SSRC generations from one sender session,
never reuses retired SSRCs, and does not automatically accept a foreign session
after a sender-process restart. These are software and finite-run consistency
properties, not a production restart manager or physical sync measurement.

Still required before production: measured clock residuals under load and
provider loss; managed provider/sender-instance agreement across process
restarts; live original-anchor and PCM correspondence; deadline-bounded loss
recovery; one validated audio rate-matching owner with owned bounded PCM; and
hardware A/V measurement across restarts and long runs. Device content latency
still requires nonuniform-marker calibration. Synchronizing computer clocks
cannot discover that content delay or genlock the independent audio and video
oscillators. The private-link RTP metadata and peer filtering are not
authentication or encryption.
