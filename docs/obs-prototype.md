# Experimental OBS synthetic adapter

This module is a feasibility prototype, not a replacement for a working capture
or audio bridge. It does not access cameras, microphones, sound devices, network
ports, OBS collections, existing source names, or startup configuration. Its only
input is the local synthetic producer's IPC mapping.

## Build and isolate

Requirements: Linux, C++20, CMake 3.24+, pthreads, pkg-config and OBS Studio 32+
development headers/library. GStreamer is not linked into OBS.

```sh
cmake -S . -B build -DAVSYNC_BUILD_IPC=ON -DAVSYNC_BUILD_OBS_PLUGIN=ON
cmake --build build --target avsync-obs
```

The module is `build/plugins/obs/avsync-obs.so`. Installing it in a normal OBS
profile is deliberately a separate maintenance operation. Prefer an isolated
libOBS smoke harness or disposable OBS configuration first. Do not open the
physical capture device in these tests. The shader is embedded; no module data
directory is required.

Source type IDs and settings:

| Type | Purpose |
| --- | --- |
| `avsync_prototype_video` | Synchronous, timestamp-selected NV12 video |
| `avsync_prototype_desktop` | Independent stereo PCM audio input |
| `avsync_prototype_microphone` | Independent mono PCM audio input |

Each has one setting, `ipc_path`, pointing to the same producer mapping. The
default is `$XDG_RUNTIME_DIR/av-sync-bridge.ipc`, matching the producer. If the
runtime variable is absent or empty, set `ipc_path` explicitly; there is no shared
`/tmp` fallback. These types do not create sources or rename existing sources by
themselves. For a test, add instances with clearly synthetic names.

## Implemented boundary

- A background worker opens/reconnects video IPC; OBS video callbacks only
  attempt nonblocking locks and bounded reads. Mapping teardown is worker-side.
- Each video tick selects the newest unconsumed frame whose presentation
  timestamp is due at `obs_get_video_frame_time()`. It does not use OBS's
  asynchronous-video queue or an OBS delay filter.
- The source uploads two NV12 planes and draws an original shader. The contract
  is **8-bit SDR, BT.709 limited range**. HDR, other matrices and other ranges
  are not supported. Pixel-level color verification is a required test, not an
  assumed property of compilation.
- Width/height come from validated IPC configuration. Video older than 250 ms
  becomes transparent, rather than freezing indefinitely.
- Desktop and microphone each have a dedicated worker and Reader. They submit
  interleaved float PCM with `obs_source_output_audio`, preserving ordinary OBS
  mixer controls and audio filters. Workers do not stop when their source is
  hidden; the OBS scene graph still controls whether audio reaches the output.
  This prototype accepts 480-frame PCM blocks at 48 kHz only.
- PCM timestamps are the daemon's absolute `CLOCK_MONOTONIC` presentation
  timestamps, never a per-source arrival-time epoch. Future-dated blocks are
  rejected, old blocks are dropped, and no multi-second delay is held in OBS.
- Audio dispatch eligibility stays anchored to the producer's sample grid;
  repeatedly sleeping a nominal block duration would accumulate scheduler error.
- After audio has started, a missing block produces timestamped silence. Short
  gaps are filled with a bounded number of silent blocks, because OBS otherwise
  smooths a sub-70-ms timestamp gap against the old sample count. A long missing
  span is skipped rather than replayed rapidly. This discontinuity policy is
  experimental: the resulting OBS mix/encoder behavior needs testing, including
  fractional-block phase changes when a producer starts a new epoch.
- Source destruction stops and joins its worker before freeing source data.
  Mapping reconnection does not require restarting OBS.

## Microphone privacy prototype

The microphone worker observes OBS source mute state. On a detected mute or
unmute transition, it records a monotonic capture cutoff. It keeps consuming the
ring while muted and submits silence; after unmute, samples captured before the
cutoff remain silent. The video and desktop clocks continue independently.

This is **not yet a privacy guarantee**. Polling can miss very short toggles;
filters and OBS may already contain audio; a submitted/encoded packet cannot be
retracted. No undocumented OBS buffer-reset API is used. A daemon-wide generation
flush/control protocol is not implemented by this adapter. Test rapid toggles,
filter history, queued packets and reconnect-while-muted before real microphone
use. The synthetic path is the only supported input for this milestone.

## Required validation and result labels

Keep these separate when reporting progress:

1. **Compilation:** module links against the target OBS installation.
2. **Module integration:** libOBS loads it; the shader compiles; all three source
   types produce media and shut down cleanly.
3. **Synthetic timing:** encoded irregular flash/click events retain their common
   timeline across staggered source creation, hide/show, stalls and reconnects.
4. **Audio controls:** independent mute, volume and routing work, including a
   rapid-toggle test with the intended microphone filters.
5. **Physical end-to-end:** not implemented or proven by any of the above.

Do not infer synchronized encoded output from timestamps at the IPC boundary.
Compare encoded-frame event times with encoded PCM event times, and report
missing events separately from offset/drift. A correct isolated synthetic result
does not prove USB capture timestamps, network timestamps or restart recovery.

### Initial integration status

The Linux module has compiled against OBS 32.2.2 and an isolated libOBS process
has loaded all three types and connected them to synthetic IPC. After resolving
test-harness mux-helper discovery and canvas cleanup, a synthetic recording
completed with clean source destruction. Encoded event measurement is now
implemented, but repeated runs exposed timing shifts and one extra detected
audio onset. The repeatability gate is **not passed**. See the
[initial validation report](validation-2026-09-15.md) for measurements and limits;
the failure/restart matrix remains a separate gate.

OBS 32.2 canvas ownership matters in standalone harnesses: removing output
channels and releasing a public scene handle may leave a canvas-owned reference.
Remove the test scene from its canvas and drain the destruction queue before
shutting down libOBS, so module-owned sources are destroyed before module unload.

At 3840x2160, one NV12 frame occupies 12,441,600 bytes. A two-second 60-fps ring
requires about 1.39 GiB, before adapter copies/metadata. One complete copy pass is
about 0.75 GB/s of payload, with additional memory read/write and GPU-upload
traffic. These are calculations, not measured performance.

## Relevant upstream contracts

- [OBS source implementation, 32.2.2](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-source.c)
- [OBS public API, 32.2.2](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs.h)
- [Linux monotonic clock implementation](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/util/platform-nix.c)

The two-second direct-audio timestamp threshold and small-discontinuity smoothing
are OBS implementation details, not stability guarantees for all future versions.
