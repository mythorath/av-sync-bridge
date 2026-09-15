# Experimental Windows desktop sender

This is a bounded capture/transport milestone, not a replacement for an existing audio bridge. It captures only the default Windows render/console endpoint. It does not open a microphone, write a recording, install a startup task, change an endpoint, or modify OBS. It is intentionally limited to short runs while the clock and transport contracts are independently checked.

## Explicit invocation

```text
avsync-windows-sender --help
avsync-windows-sender --loopback --host IPV4 --clock-port CLOCK_PORT --rtp-port RTP_PORT --rtcp-port RTCP_PORT --seconds 15
```

Replace the uppercase placeholders with the explicitly selected destination and three distinct ports. All run options are mandatory. `--seconds` is an overall observation deadline of 1–120 seconds, including clock acquisition and any pipeline rebuilds; retries do not extend it. Help, no arguments, and invalid arguments open no endpoint and send no network traffic. Only numeric unicast IPv4 destinations are accepted in this version.

A deliberate run **captures desktop PCM and sends it unencrypted to the chosen host**. It is suitable only for an explicitly trusted, isolated test network. This prototype has no authentication, encryption, retransmission, congestion control, microphone privacy integration, or automatic reconnection to a different endpoint. GStreamer/driver calls and shutdown are not forcibly interrupted if the operating system stalls, so the deadline is not a hard real-time process watchdog.

## Capture and conversion

The WASAPI client requests shared-mode loopback using `GetMixFormat`, without changing that format. It retains each `GetBuffer` packet's device-frame position and paired QPC timestamp. It copies one bounded packet to scratch storage and releases the WASAPI packet before GStreamer allocation, clock mapping, or network work. Silence flags produce correctly encoded silence; they do not mean missing media. The initial discontinuity flag is counted separately. [Microsoft loopback recording](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording), [GetMixFormat](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudioclient-getmixformat), [GetBuffer](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)

Supported input formats are interleaved float32/float64 or full-width integer PCM 8/16/24/32-bit, 8–384 kHz, and at most eight channels with a supported speaker layout. PCM with fewer valid bits than its container, unknown subformats, unknown speaker bits, and unpositioned multichannel input are rejected. This is deliberately narrower than all possible Windows audio formats.
The precise nominal converter additionally requires
`48000 / gcd(input_rate, 48000) <= 640`, which includes ordinary audio rates but
rejects pathological coprime rates to bound coefficient-table growth.

The conversion path is:

```text
WASAPI PCM packet + separate original device-frame/QPC anchor
  -> timestamp-free nominal converter with explicit normalized stereo matrix
  -> continuous 48 kHz stereo float, with an explicit output-frame ledger
  -> bounded appsrc, nominal sample-count timestamps
  -> audioconvert, 48 kHz stereo S24BE
  -> rtpL24pay -> rtpbin -> RTP and RTCP UDP outputs
```

The project downmix preserves left/right balance, excludes the LFE channel, and uses one common gain with 10% headroom for bounded full-scale inputs. It is a project policy, not a claim of a standardized broadcast mix. Windows and GStreamer speaker-mask bits are **not interchangeable**; the sender derives GStreamer caps from explicit positions. See [audio conversion policy and fixtures](audio-conversion.md). The final 24-bit conversion disables dither/noise shaping for deterministic initial fixtures; downmix, sample-rate conversion, and quantization are not bit-perfect preservation of the original multichannel float stream. [audioconvert](https://gstreamer.freedesktop.org/documentation/audioconvert/index.html), [audioresample](https://gstreamer.freedesktop.org/documentation/audioresample/index.html), [L24 payloader format](https://gstreamer.freedesktop.org/documentation/rtp/rtpL24pay.html)

## Clock and wire contract

The Linux test receiver provides a monotonic media clock through `GstNetTimeProvider`. The Windows sender obtains it through `GstNetClientClock`. This changes no operating-system clock. Startup requires both a synchronized client and the helper's explicit health checks. The raw Windows QPC / GStreamer internal-clock relationship is checked at runtime. Historical WASAPI QPC values are mapped through the **underlying network clock's calibration**, not through the wrapper's already adjusted time and not through packet arrival time. See [network clock helper](network-clock.md) and [GStreamer network clocks](https://gstreamer.freedesktop.org/documentation/net/gstnetclientclock.html).

The sender's pipeline uses base time zero and start time `GST_CLOCK_TIME_NONE`.
Appsrc has `do-timestamp=false`. Its timestamps now follow an explicit rational
48 kHz output-frame timeline anchored once to the first mapped capture timestamp
of each generation. The raw converter receives no timestamps and cannot reset its
filter because the device clock drifts. Its retained filter tail is discarded on
stop/reset, not flushed with invented live silence. There is no presentation delay.

**These nominal RTP/RTCP timestamps are not original capture timestamps.** Original
device-position/mapped-QPC anchors are checked separately in the sender and used
only for diagnostic rate estimates. They are **not transmitted yet**. The receiver
therefore cannot infer the device clock from nominal RTP progression or use it for
adaptive correction. See [nominal conversion validation](nominal-audio-validation.md)
and the [old conversion failure](conversion-timing.md). Physical content-time
calibration remains required even after anchor transport is implemented.

A WASAPI loopback packet can be available before its endpoint timestamp. A bounded diagnostic comparison confirmed that such a lead can already exist between the original QPC timestamp and the raw local clock, rather than being introduced by network-clock conversion. This observation does not establish correct physical playback timing, speaker latency, or A/V alignment. The sender therefore accepts original timestamps at most 100 ms ahead of the shared clock or 100 ms old, including the exact boundaries. It applies the same pure policy before and after pipeline construction. It never subtracts a guessed device latency, adds an offset, or replaces the timestamp with the current time. Clock acquisition, calibration validity, and health gates remain mandatory.

| Wire field | Contract |
| --- | --- |
| RTP payload | Dynamic payload type 96, `L24`, 48,000 Hz, stereo |
| RTP packetization | MTU 1,200 bytes, maximum packet time 4 ms |
| RTCP time source | `ntp-time-source=clock-time` |
| RTCP correspondence | `rtcp-sync-send-time=false`: nominal media time, not original capture time or transmission latency |
| RTCP cadence | Internal session minimum interval 500 ms; not a guaranteed first-report deadline |
| Time domain | Linux shared monotonic clock represented in RTCP's 32.32 field, **not UTC/NTP wall time** |
| Epoch reset | Fresh SSRC and fresh conversion/RTP pipeline; no old PCM drained into it |

The paired receiver must understand this explicit shared-monotonic convention. It stays in **priming** until it obtains a validated sender-report/reference timestamp mapping; it must not substitute packet arrival time. This is not a generic wall-clock RTP interoperability claim. The sender does not yet receive RTCP feedback or implement RTX. [rtpbin properties](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpbin.html), [GStreamer 1.28.6 capture-time/clock-time correspondence](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-good/gst/rtpmanager/gstrtpsession.c#L2326)

Each pipeline uses a neutral generated RTCP CNAME. It does not send GStreamer's default host-derived CNAME or report endpoint identities. SSRCs are session identifiers, not credentials or authentication.

## Bounds and discontinuities

Appsrc has a 100 ms / format-derived byte bound, a 32-buffer count bound, nonblocking pushes, and upstream leaking as a final capacity guard. Reaching `enough-data` triggers a pipeline reset rather than silently accepting a growing backlog. A packet already executing downstream and the operating-system UDP buffers are outside that appsrc queue; their limits and actual behavior still require load/loss testing. Requested UDP send buffers are 16 KiB. [appsrc queue controls](https://gstreamer.freedesktop.org/documentation/app/appsrc.html)

A post-start WASAPI discontinuity, device-position gap, uncertain timestamp, unhealthy clock, invalid/nonmonotonic mapped time, capture age over 100 ms, capture time more than 100 ms ahead, or appsrc overflow causes dropping/resetting, not timestamp rebasing. After clock health is lost, reacquisition requires a fresh observation window; old observation counts do not qualify it. At most eight resets are allowed within the original deadline. The first packet of a fresh pipeline is explicitly discontinuous. Endpoint invalidation or pipeline failure exits with an error; it never guesses a new endpoint. Stop/reset drops queued PCM rather than waiting for old audio to play.

These are fail-visible prototype policies, not proven seamless recovery. In
particular, **fixed-rate conversion is not adaptive device-clock correction**.
A sound device can drift against the shared clock even when network timing is
accurate. A bounded ASRC backend is tested offline, but its live controller,
original-anchor transport and combined real A/V path remain unimplemented.

## Output, build, and validation gates

The final JSON contains format metadata, capture/mapping counters, resets/drops, clock-health observations, paired first/last device/QPC/mapped timestamps, and RTP/RTCP output counters. `rtp_packets_at_output` is observed at the sender's output pad **before** the UDP sink; it is not proof of socket delivery or receiver playback. `last_sr_clock_32_32` uses the agreed monotonic convention, not wall time. Raw clock RTT and observation age are diagnostics, not a guaranteed synchronization-error bound.

Rejection diagnostics distinguish invalid mapping, invalid current clock, future beyond the bound, stale data, nonmonotonic data, unhealthy clock, and post-build deadline/window failures. They retain at most the first eight rejected-packet examples plus the final rejection, and aggregate timestamp-difference ranges. The calibration snapshot is taken after the actual mapping and can differ if a concurrent calibration update occurs. No rejected PCM is logged. The pure capture-window tests cover exact boundaries, invalid inputs, asymmetric limits, near-overflow values, and consistent pre/post-work decisions without opening an endpoint or network connection.

Exit code 0 means RTP output was observed (or help was shown), 1 means a capture/clock/pipeline failure, 2 means invalid arguments, and 3 means waiting for usable clock or RTP. An empty or silent source is never presented as a successful audible-signal test.
Exit 3 also reports invalid nominal RTP timing: malformed/empty payload, frame
progression mismatch, or a nominal PTS difference greater than 1 ns. Additional
counters expose conversion frame totals, processing maximum, original-anchor
rate estimates and post-conversion age rejection. These are diagnostic gates,
not proof of remote reception, perceptual quality or physical synchronization.

The C++20 source is `apps/windows_sender.cpp`. The Windows target is `avsync-windows-sender`, enabled by `AVSYNC_BUILD_NETWORK=ON`. It requires the matching MSVC GStreamer development SDK (1.24 or later), the project `avsync_gst_clock` and `avsync_nominal_audio` helpers, `gstreamer-1.0`, `gstreamer-app-1.0`, `gstreamer-audio-1.0`, `gstreamer-net-1.0`, `gstreamer-rtp-1.0`, `gio-2.0`, and Windows `ole32`/`uuid`. Required runtime factories are `appsrc`, `audioconvert`, `capsfilter`, `rtpL24pay`, `rtpbin`, and `udpsink`. No live capture/network test is part of a default help test.

With the matching x64 MSVC toolchain, CMake, and `pkg-config` available, configure the SDK's runtime DLL/plugin paths and `pkg-config` search path according to its installation. Then, from the repository root:

```powershell
cmake -S . -B build/windows-network -G "Visual Studio 17 2022" -A x64 -DAVSYNC_BUILD_NETWORK=ON -DAVSYNC_BUILD_IPC=OFF -DBUILD_TESTING=ON
cmake --build build/windows-network --config Release
ctest --test-dir build/windows-network -C Release --output-on-failure
.\build\windows-network\Release\avsync-windows-sender.exe --help
```

These commands build and run non-capture tests. A network/audio run still requires the explicit full invocation above and a separately prepared matching receiver.

Compilation and the following integration gates must be tracked separately: explicit matrix/alias/clipping fixtures; actual wire SR correspondence; receiver reference timestamps; packet loss/reordering; clock-provider loss; bounded restart behavior; sustained device-clock drift; and eventual independent encoded OBS A/V measurements. Source code or sender counters alone cannot pass those gates. No host-specific results belong in this public document.
