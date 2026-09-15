# AV Sync Bridge

An experimental, clock-aware synchronization bridge for independently captured
video and network audio, designed for OBS Studio.

**Early development — not a working replacement for a production audio bridge.**
The first milestone is an isolated synthetic timing prototype. Windows capture,
network-clock integration, packet recovery, physical video ingest and production
recovery must each pass explicit validation before a release is recommended.

## Intended use

Keep a gaming PC's multichannel speaker output while sending a stereo copy and a
separate microphone to a Linux OBS machine receiving USB/HDMI capture video. The
goal is one timestamp-based playout schedule, bounded buffers, smooth rate matching
and repeatable recovery, while retaining separate OBS mixer controls.

It is not specific to one capture-card brand, private network, audio endpoint, or
scene collection. Shared clocks cannot infer unknown delay before a device's
capture timestamp; an initial calibration remains necessary.

## Architecture

- Windows capture agent: timestamped desktop and microphone PCM.
- Linux timing service: video capture, audio reception, shared media clock,
  bounded playout and per-input rate correction.
- Thin OBS adapter: separate video, desktop-audio and microphone sources sharing
  one presentation timeline.

The initial design targets a configurable two-second synchronization delay, not
two seconds stacked on existing audio buffering. The long delay stays outside OBS.

## Development

Portable core: C++20 and CMake 3.24 or newer. Linux service and OBS integration are
optional. Build instructions and test coverage will be added with each working
component; the roadmap must distinguish implemented work from proposed design.

Do not install untested components into your normal OBS profile. Use an isolated
configuration with synthetic sources first. Never publish real device identifiers,
credentials, recordings or private scene collections in bug reports.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). OBS Studio and other dependencies retain
their own licenses. This project is independent of the OBS Project and capture
hardware vendors.
