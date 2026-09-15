# Build and isolated testing

## Core and Linux IPC

Requirements: CMake >=3.24 and a C++20 compiler. MSVC and GCC are initial targets.
The core has no third-party dependencies. Linux IPC additionally uses POSIX file
mapping and pthreads; it is not available on Windows.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release --parallel 2
ctest --test-dir build -C Release --output-on-failure
```

For a core-only build on either platform add `-DAVSYNC_BUILD_IPC=OFF`. Linux IPC is
enabled by default on Linux and disabled on other systems. The default Windows
build includes the optional metadata-only WASAPI
probe (`AVSYNC_BUILD_WINDOWS_PROBE=ON` by default on Windows); its help test does
not open an endpoint. See [explicit probe use](windows-capture.md). No external
test framework is downloaded.

## Optional network diagnostics

`AVSYNC_BUILD_NETWORK=ON` adds the desktop-only Windows sender or the Linux
diagnostic receiver, plus shared-clock tests. It is **off by default** and does
not install a service or change OBS. Requires GStreamer >=1.24 development
libraries for core, net, app, audio and RTP, plus GIO and pkg-config. Runtime
plugins must include appsrc/appsink, audioconvert, audioresample, rtpbin, UDP,
and L24 payload/depayload elements. These are bounded experiments, not yet a
production audio replacement. See [sender](windows-sender.md),
[clock contract](network-clock.md) and [receiver](network-receiver.md).

Ubuntu development packages are `libgstreamer1.0-dev` and
`libgstreamer-plugins-base1.0-dev`. Review package-manager changes before installing
on a live machine. Then:

```sh
cmake -S . -B build-network -DCMAKE_BUILD_TYPE=Release -DAVSYNC_BUILD_NETWORK=ON
cmake --build build-network --parallel 2
ctest --test-dir build-network --output-on-failure
```

On Windows use a matching x64 MSVC GStreamer **development** distribution. A
runtime-only install lacks headers/import libraries. The tested 1.28.6 Inno
installer supports `/portable=1 /CURRENTUSER /TYPE=devel /DIR="NEW_SDK_DIRECTORY"`;
portable mode avoids registry/environment/Visual Studio changes. Obtain installers
and checksums from the [official download directory](https://gstreamer.freedesktop.org/data/pkg/windows/).
Do not replace a functioning production runtime just to build this experiment.

In the build shell only, prepend that SDK's `bin` to PATH and set PKG_CONFIG_PATH
to its `lib/pkgconfig`. Configure with Visual Studio 2022 x64 and the network
option above. Use `--config Release` for building and `-C Release` for CTest.
Run diagnostics from a shell with the same process-local SDK PATH; avoid mixing
DLLs/plugins from different installations. The normal CI Windows job tests the
dependency-free core/probe, not the optional SDK sender. Linux CI compiles the
receiver and runs pure clock tests but does not open network listeners.

Optional Linux memory/undefined-behavior checks:

```sh
cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DAVSYNC_ENABLE_SANITIZERS=ON
cmake --build build-asan --parallel 2
ctest --test-dir build-asan --output-on-failure
```

Use a private user-owned runtime directory for IPC. Do not write capture buffers
to a network share. For example, in two shells using the **same** directory:

```sh
mkdir -m 700 "$XDG_RUNTIME_DIR/avsync-demo"
./build/avsync-synthetic --path "$XDG_RUNTIME_DIR/avsync-demo/media.ipc" --duration 10
```

```sh
./build/avsync-probe --path "$XDG_RUNTIME_DIR/avsync-demo/media.ipc" --duration 8 --verify --delay-ms 2000
```

Choose a new directory if it already exists. See each tool's `--help`. The probe
checks IPC metadata/delivery, not actual encoded picture-to-sound alignment.
Default media is 640x360 NV12 at 60 fps with two 48 kHz PCM tracks; high-resolution
resource use is not yet validated. The producer exits after capture duration plus
buffer drain; it never registers a startup service.

## Optional OBS prototype

Requires matching OBS development headers/libraries, pkg-config, and Linux.
The module requires libobs >=32.0; the standalone test harness requires >=32.2
for explicit canvas cleanup. Tested versions are listed in validation reports;
minimum-version checks are not a compatibility certification.

Some distro `libobs-dev` packages are older than the required runtime. Do not
replace a functioning OBS installation merely to run this test. Make sure
`pkg-config --modversion libobs` resolves to the intended development installation.
The headers also need SIMDe (Ubuntu package `libsimde-dev`). The smoke harness
uses X11 development headers, Xvfb, the installed OBS x264/FFmpeg modules and the
installed `obs-ffmpeg-mux` executable on PATH.

```sh
cmake -S . -B build-obs -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DAVSYNC_BUILD_OBS_PLUGIN=ON -DAVSYNC_BUILD_OBS_SMOKE=ON
cmake --build build-obs --parallel 2
```

This builds a module but **does not install it into your OBS configuration**.
No `sudo cmake --install` is needed for the isolated test. The harness links the
existing OBS mux helper beside its executable inside the build directory.

## Encoded smoke test

Start the synthetic producer for at least 22 seconds in a private directory.
Immediately run the following on an isolated X display. Replace the module/data
paths with those from your OBS installation. Do not point it at a real audio
bridge, microphone, recording or production OBS profile.

```sh
xvfb-run -a -s '-screen 0 800x600x24' ./build-obs/avsync-obs-smoke \
  "$XDG_RUNTIME_DIR/avsync-demo/media.ipc" ./synthetic-test.mkv \
  ./build-obs/plugins/obs/avsync-obs.so \
  /usr/lib/x86_64-linux-gnu/obs-plugins /usr/share/obs/obs-plugins 18
```

The harness loads only the prototype and OBS's x264/FFmpeg modules; creates a
640x360/60 composition and two separately encoded audio tracks; records a bounded
duration; then stops and removes its temporary sources. It uses libOBS, not the
normal OBS GUI or saved profiles. It refuses to overwrite an existing output.
Expect a file containing **generated** flashes and tones, not your desktop.

Inspect decoded presentation times and uniquely spaced events with Python 3,
FFmpeg and ffprobe installed:

```sh
python3 tools/measure_synthetic.py ./synthetic-test.mkv
python3 -m unittest discover -s tests -p 'test_*.py'
```

The helper returns zero for a valid marker match and any requested timing gates,
two for a marker mismatch, three for a timing-gate failure, and one for a
decoding/processing error. Without timing limits, **a valid match does not assert
acceptable sync**. Missing/extra markers are rejected, not matched to a convenient
subset or repeating cycle. Headless Xvfb/CPU rendering is not a 4K60 hardware
performance benchmark. See [initial validation](validation-2026-09-15.md).

```sh
python3 tools/measure_synthetic.py ./synthetic-test.mkv \
  --max-median-ms 16.667 --max-offset-ms 33.333
```

`--cycles N` explicitly requires all six markers in each of N complete cycles,
including their inter-cycle spacing (1..6 cycles, maximum 120-second recording).
It does not search for a cycle shift or discard troublesome events. Region
boundaries are included to expose split tones. Median limits apply to each
track's absolute median; maximum limits apply to every marker's absolute offset.

## Repeatable synthetic regression suite

The Linux-only runner creates an isolated display and starts only the project's
test producer/harness. It never attaches to the OBS GUI. Supply a **new** output
directory inside an existing trusted parent; recordings and logs are private and
must not be committed. Replace the module/data paths to match your installation.

```sh
python3 tools/run_synthetic_suite.py --build-dir ./build-obs \
  --obs-plugins /usr/lib/x86_64-linux-gnu/obs-plugins \
  --obs-data /usr/share/obs/obs-plugins \
  --output-dir "$XDG_RUNTIME_DIR/avsync-regression-new"
```

The default is three fresh baseline recordings with the explicit timing gates
above. Every run writes `measurement.json`, `obs.log`, generated `encoded.mkv`,
and pre-encoder mixer RMS traces (`.mix0.csv` / `.mix1.csv`). Raw traces use
absolute monotonic sample timestamps; the harness's recording-start log is **not**
the encoded file's zero point. Callbacks append to bounded preallocated memory;
CSV files are written only after callbacks have been disconnected. The runner
also requires a 2 ms absolute raw-mixer check against the producer's known
presentation schedule, independently of encoded relative A/V alignment. The
intentionally muted mic uses the separate known-event assertion described below.

Repeat `--case` to select specific scenarios:

| Case | What it exercises |
| --- | --- |
| `baseline` | Fresh producer and OBS harness, 40 ms handoff lead |
| `lead-zero` | Diagnostic control with no handoff lead; failures are retained |
| `restart` | Stop a first producer, keep OBS sources alive, then start a new epoch before measuring a complete new cycle |
| `stall` | Pause only the generated producer for 400 ms; no physical device or production service is signaled |
| `stagger` | Video, desktop and mic source creation staggered while producer starts |
| `hide` | Hide/show synthetic video between markers without resetting its clock |
| `mute` | Mute/unmute mic; buffered fixture event 3 must not replay |
| `rapid-mute` | Immediate mute/unmute in one harness iteration; same expected cutoff |

Mute fixtures intentionally have five mic markers and six desktop/video markers.
The runner checks the **known** suppressed event rather than inferring a subset,
and applies both median and maximum timing gates to the remaining events. These
fixtures do not establish privacy through arbitrary real audio filters.

`--cycles 3` runs a roughly one-minute multi-cycle check (`mute` and `rapid-mute`
currently require one cycle). This is not a 30-minute load test. The runner
cleans up only processes it creates and reports cleanup failures as failures.
