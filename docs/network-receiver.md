# Bounded network receiver diagnostics

The optional Linux `avsync-network-receiver` provides a shared monotonic clock
and inspects desktop PCM received through RTP/RTCP. It never opens an audio
device, saves PCM, changes OBS, or feeds the IPC playout bridge. This is a
transport/timestamp experiment, not an audible or physical A/V validation.

```text
avsync-network-receiver --bind LOCAL_IPV4 --peer SENDER_IPV4 --clock-port CLOCK_PORT --rtp-port RTP_PORT --rtcp-port RTCP_PORT --seconds 30 --expect-media
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

Each received SSRC gets a depayloader and a bounded appsink. At most eight
sessions/report slots are accepted per finite process; further sessions fail
visibly. Stop the diagnostic before restarting a production component, rather
than making this session budget an unattended service.

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

Exit 0 means help or completed diagnostics. Exit 1 means configuration/pipeline
failure. With `--expect-media`, exit 3 means no timed media, stale/invalid
references, missing reference metadata after lock, or nonmonotonic timestamps.
That option does **not** fail on all
continuity gaps, startup untimed packets or silence: `timestamps_observed` is
deliberately weaker than `continuity_passed` or `audio_quality_passed`. Keep full
reports when comparing tests. Durations are 1..180 seconds; external driver/OS
shutdown stalls are not a hard-real-time watchdog guarantee.

Runtime dependencies include rtpbin, udpsrc, rtpL24depay and appsink. Build with
`AVSYNC_BUILD_NETWORK=ON`; see [building](building.md). Help/default CTest does
not bind sockets or capture audio. Do not publish local addresses, hostnames or
raw device logs in public bug reports.

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
