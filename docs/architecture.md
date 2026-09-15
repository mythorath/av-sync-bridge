# Architecture and invariants

## Target system

An independent Windows desktop/mic capture agent sends timestamped PCM to a Linux
timing service. The service also owns a local video capture input. A thin OBS
module presents synchronized picture, desktop sound and mic with independent
mixer controls. Local multichannel speaker playback is not changed.

The project must not depend on one user's machine, LAN address, capture-card
brand, OBS scene collection or startup scripts. Version one targets Windows audio
and Linux OBS, rather than claiming universal OS/device support.

## Time

Keep original capture time and final presentation time separate. Signed integer
nanoseconds are validated before arithmetic. Each connection carries a shared
session plus per-stream generation; a reconnect may invalidate one stream without
restarting unrelated streams.

The network clock is Linux's monotonic media clock exposed through
GStreamer NetTimeProvider/NetClientClock. A bounded desktop-only RTP/RTCP PCM
diagnostic now implements that boundary separately from synthetic IPC/OBS;
see [network validation](network-validation.md). Deadline-bounded RTX and the
complete playout integration are not implemented. OS NTP alone cannot reconstruct
discarded capture metadata.

Given calibrated content time T, present at T + configured playout delay. The
initial design target is two seconds, including network jitter/recovery allowance,
not an extra delay stacked over an old receiver. Calibration accounts for fixed
capture-path differences. Unknown variable delays before a device timestamps
content remain a feasibility risk.

## Scheduling and memory

Choose frames by presentation deadline, not by a frame count learned at startup.
Bound every queue by count, bytes and/or time. Discard expired data; never play a
backlog faster to catch up. Video may repeat/drop at output cadence, while audio
needs bounded repair/concealment and smooth sample-rate correction.

Keep long raw-frame storage in system RAM outside OBS. Two seconds of 3840x2160
NV12 at 60 fps is approximately 1.39 GiB before overhead; this is a calculation,
not a measured implementation footprint. Release capture buffers promptly and
copy into owned storage. Avoid GPU texture queues or another encode merely to
create a delay.

## OBS boundary

An independently rebased OBS source can undo upstream synchronization. The first
proof uses a synchronous render-tick source and separate ordinary audio sources,
all mapped to the same Linux monotonic presentation time. Audio enters OBS near
its deadline, not carrying a timestamp from seconds before delivery. The synthetic
adapter now makes PCM available with a bounded 40 ms first-sample handoff lead
(up to 50 ms including a block's tail); its final presentation timestamps are
unchanged. This is mixer availability headroom, not a new path-delay offset.

No render callback waits for networking or IPC. Missing output is explicit.
One service owns physical capture; development must not compete with another
process for a USB device. Source hiding and scene switching must not reset time.
Normal OBS audio submission is retained to preserve its filter/mixer behavior.

## Recovery and privacy

States must distinguish waiting, clock acquisition, priming, locked, degraded and
re-syncing. "Locked" describes timing machinery, not automatic knowledge of
semantic lip sync. Silence is valid media; it must not be confused with absence.

Mute invalidates the mic's delayed generation, with timestamped silence continuing.
After unmute, only newly captured samples may enter the long buffer. Test rapid
toggle behavior through OBS and all filters before claiming privacy. Already
encoded or transmitted audio cannot be retracted. A mic failure must not halt
the desktop/video pair.

No automatic public streaming, destructive migration or system-clock change.
Any deployment needs snapshots, explicit capture ownership, a maintenance window,
and a tested rollback. The synthetic milestone does not provide this deployment.

## References

- [OBS source API](https://docs.obsproject.com/reference-sources)
- [V4L2 capture timestamp definition](https://docs.kernel.org/userspace-api/media/v4l/buffer.html)
- [WASAPI capture time](https://learn.microsoft.com/en-us/windows/win32/api/audioclient/nf-audioclient-iaudiocaptureclient-getbuffer)
- [GStreamer network clocks](https://gstreamer.freedesktop.org/documentation/net/gstnetclientclock.html)
- [RTP synchronization](https://gstreamer.freedesktop.org/documentation/rtpmanager/rtpbin.html)
