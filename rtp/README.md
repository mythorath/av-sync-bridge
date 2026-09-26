# Production path: RTP desktop audio

Files for the always-on desktop audio path described in
[docs/rtp-path.md](../docs/rtp-path.md).

| Folder | What it is |
|---|---|
| `windows/desktop_rtp_sender.py` | Supervised GStreamer sender: WASAPI loopback, 7.1 to stereo downmix, RTP L24 |
| `windows/install-sender-task.ps1` | Registers the sender as a self-reviving scheduled task |
| `windows/run_sync_test.py` | Fully automatic A/V sync measurement against the six-marker reference |
| `linux/windows-desktop-rtp.conf` | PipeWire `module-rtp-source` receiver (drift-compensating jitter buffer) |
| `linux/avsync-rtp-receiver.service` | systemd user unit for the receiver |
| `linux/pipewire-pulse.conf.d/` | The `avsync_desktop` null sink OBS records from |
| `linux/promote-obs-audio-thread.sh` | Gives OBS's PipeWire audio thread realtime priority |
| `obs/*.patch` | Two-line patch to obs-pipewire-audio-capture: graph-clock timestamps, realtime processing |
