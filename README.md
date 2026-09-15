# AV Sync Bridge

An experimental, clock-aware synchronization bridge for independently captured
video and network audio, designed for OBS Studio.

**Early development — not a working replacement for a production audio bridge.**
The isolated synthetic prototype now passes repeated encoded timing checks and
controlled restart/stall/mute fixtures. See [measured results and limits](docs/handoff-validation.md).
An optional [desktop-only Windows sender](docs/windows-sender.md) now feeds a
[Linux diagnostic receiver](docs/network-receiver.md) using a shared monotonic
clock and RTP/RTCP PCM. Short real-network starts and a controlled clock outage
have been tested; see [transport results and limits](docs/network-validation.md).
This path is not yet connected to the OBS playout bridge. Packet recovery,
adaptive audio-rate correction, physical video ingest and production recovery
still require explicit validation. A separate [metadata-only WASAPI probe](docs/windows-capture.md)
remains available without transmitting PCM.

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

Portable core: C++20 and CMake 3.24 or newer. Linux IPC and OBS integration are
optional. See [build instructions](docs/building.md), [architecture](docs/architecture.md),
[timing contracts](docs/timing.md), [IPC](docs/ipc.md), and [roadmap](docs/roadmap.md).

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

The default build includes the portable core, plus synthetic IPC tools on Linux.
It does **not** build/install an OBS plugin, capture real media, open network ports,
change startup tasks, or touch an OBS profile. Tests remain active in Release builds.

Implemented foundation: timestamp arithmetic, rational cadence, bounded queues,
rate-estimation recommendations, privacy-cutoff logic, local synthetic IPC and an
optional native OBS adapter. A rate estimate is not implemented ASRC; simulated
audio is not hardware validation. The optional network experiment remains
separate from the synthetic IPC/OBS playout path.

Do not install untested components into your normal OBS profile. Use an isolated
configuration with synthetic sources first. Never publish real device identifiers,
credentials, recordings or private scene collections in bug reports.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). OBS Studio and other dependencies retain
their own licenses. This project is independent of the OBS Project and capture
hardware vendors.
