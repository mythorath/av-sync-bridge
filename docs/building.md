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
enabled by default on Linux and disabled on other systems. The Windows build does
not yet contain an audio sender. No external test framework is downloaded.

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

Use the measurement helper when available to inspect decoded presentation times
and uniquely spaced events. A successful recording alone does not establish sync.
Headless Xvfb/CPU rendering is not a 4K60 hardware performance benchmark.
