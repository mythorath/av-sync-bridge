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
An optional [physical video capture and memory buffer](docs/video-buffer.md) now
preserves NV12 frames and their driver timestamps in a bounded two-second ring;
see [physical video results and remaining faults](docs/video-validation.md).
These paths are now connected for [isolated combined physical tests](docs/physical-combined-validation.md),
not normal OBS operation. Following clock-freshness, video-publication and
reference-scheduling fixes, three unchanged-setting physical timing passes
succeeded. Production integration, loaded stability and whole-process/reboot
recovery remain unqualified. See the [current checkpoint](docs/checkpoint-2026-09-15.md).
A separate [metadata-only WASAPI probe](docs/windows-capture.md)
remains available without transmitting PCM.
Opt-in [same-sender recovery](docs/desktop-recovery.md) has now reconnected an
isolated OBS recording after a planned clock interruption, with a measured
silence gap. It is not whole-process or reboot recovery.

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
audio is not hardware validation. The optional network and physical-video paths
can feed separate IPC inputs to the isolated OBS harness. The
[optional resampler and original capture-anchor components](docs/asrc-validation.md)
now pass offline checks, including a 600-second generated-marker case and wrong-rate
controls. The [old sender conversion](docs/conversion-timing.md) hid drift and
stepped its timestamps under simulated clock mismatch. A new
[timestamp-free nominal converter](docs/nominal-audio-validation.md) keeps sample
progression separate from original capture-clock diagnostics. A bounded
[original-anchor transport diagnostic](docs/audio-anchor-transport-validation.md)
now preserves those records independently of nominal RTP time, with strict
sample-position and generation checks. An [offline correction worker](docs/audio-correction.md)
now combines validated PCM, original-clock estimates and bounded resampling;
see its [generated-media results and open gates](docs/audio-correction-validation.md).
A [model-based phase safeguard](docs/audio-phase-guard.md) now checks that worker
against original capture anchors before releasing output. It passes separate
generated-waveform and fault checks, not live A/V validation. The
[quality/resource and isolated live correction milestone](docs/audio-live-correction-validation.md)
now exercises actual desktop PCM through ASRC in an explicit inspect-and-discard
receiver mode. It does **not replace normal OBS audio**. Loaded clock/recovery
reliability, production calibration and continuous combined A/V delivery remain open.
The [startup and desktop handoff checkpoint](docs/startup-handoff-validation.md)
adds bounded provisional acquisition and an explicit two-second corrected-audio
IPC buffer in the native adapter's format. Real desktop audio reached an encoded
recording through that native source in an isolated OBS harness. This begins
integration; it does not install a normal OBS source or replace production audio.
The [physical marker measurement helper](docs/physical-measurement.md) requires
one complete six-event pass and checks audio/video spacing independently. It
rejects missing or obscured events rather than pairing a convenient subset.
An [instrumented browser reference](docs/browser-reference.md) generates the same
six-event pattern and reports its own scheduling uncertainty. Its report is not
a substitute for measuring the physical capture and encoded output.

Do not install untested components into your normal OBS profile. Use an isolated
configuration with synthetic sources first. Never publish real device identifiers,
credentials, recordings or private scene collections in bug reports.

## License

GPL-2.0-or-later. See [LICENSE](LICENSE). OBS Studio and other dependencies retain
their own licenses. This project is independent of the OBS Project and capture
hardware vendors.
