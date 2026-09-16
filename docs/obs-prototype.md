# Experimental native OBS adapter

This module is a feasibility prototype, not a replacement for a working capture
or audio bridge. It does not access cameras, microphones, sound devices, network
ports, OBS collections, existing source names, or startup configuration. Its only
input is an explicit local IPC mapping. Synthetic producers remain the default;
separate capture tools can provide physical media in isolated maintenance tests.

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
physical capture device in synthetic tests. Physical producers require a separate
explicit maintenance step. The shader is embedded; no module data
directory is required.

Source type IDs and settings:

| Type | Purpose |
| --- | --- |
| `avsync_prototype_video` | Synchronous, timestamp-selected NV12 video |
| `avsync_prototype_desktop` | Independent stereo PCM audio input |
| `avsync_prototype_microphone` | Independent mono PCM audio input |

Each has an `ipc_path` setting, normally pointing to the same synthetic mapping.
Separate physical producers may use different mappings, but must preserve a
common monotonic presentation timeline; IPC does not infer content offsets. The
default is `$XDG_RUNTIME_DIR/av-sync-bridge.ipc`, matching the producer. If the
runtime variable is absent or empty, set `ipc_path` explicitly; there is no shared
`/tmp` fallback. These types do not create sources or rename existing sources by
themselves. For a test, add instances with clearly synthetic names.

The two audio types also accept the developer setting `handoff_lead_ms`, an
integer from 0 to 100, default 40. Negative/out-of-range values are rejected;
invalid updates retain the previous configuration. This setting changes when
PCM becomes available to OBS, **not its final presentation time**. At 40 ms,
the first sample can be submitted 40 ms ahead; a 480-sample block adds up to
another 10 ms of queued tail. A one-sample rounding allowance is explicit.
The daemon's multi-second delay is not copied into the OBS input queue.

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
- IPC retains the daemon's original absolute `CLOCK_MONOTONIC` presentation
  timestamps. Audio is submitted on a persistent 48-kHz sample grid anchored to
  the first such timestamp, never to packet arrival. Normal contiguous packets
  keep their original dates; a new producer's fractional-block phase is rounded
  to the nearest sample, bounded by one sample, rather than moved by a block.
- The worker consumes at most eight blocks per iteration through the configured
  handoff horizon. Stale data is dropped; a bounded handoff queue protects the
  OBS mixer deadline. No multi-second future audio is submitted.
- Sample positions use quotient/remainder arithmetic, not repeated rounded
  nanosecond increments or successive wakeup times. Missing intervals up to
  100 ms receive the exact number of silent samples. Already-covered leading
  samples are trimmed with a matching reduced frame count. A fractional 10-ms
  epoch change therefore does not create overlapping PCM blocks.
- After audio starts, missing input produces continuous bounded silence, leaving
  one block of scheduling margin before filling. Long worker stalls/gaps use an
  explicit discontinuity instead of replaying a backlog. Recovery of OBS's own
  buffered state after such hard discontinuities remains a required test.
- Source destruction stops and joins its worker before freeing source data.
  Mapping reconnection does not require restarting OBS.

## Microphone privacy prototype

The microphone subscribes to OBS's source mute signal. On each mute or unmute
event, the callback records an atomic monotonic capture cutoff and state; it
does not perform IPC or wait for the audio worker. It keeps consuming the
ring while muted and submits silence; after unmute, samples captured before the
cutoff remain silent. The video and desktop clocks continue independently.

This is **not yet a privacy guarantee**. The handoff window, filters and OBS may
already contain audio; a submitted/encoded packet cannot be retracted. No
undocumented OBS buffer-reset API is used. A daemon-wide generation
flush/control protocol is not implemented by this adapter. Test rapid toggles,
filter history, queued packets and reconnect-while-muted before real microphone
use. The synthetic path is the only supported input for this milestone.

## Low-overhead diagnostics

Each audio worker writes aggregate status every five seconds and at shutdown:
media blocks, submitted samples, silence-fill samples, trimmed samples, stale
IPC skips, busy reads, invalid metadata, starvation fills, mapping reconnects,
producer generations, hard discontinuities, privacy-gated blocks and mute events.
No audio samples, file paths or capture-device identifiers are logged. Counters
are local observations: `starvation_fills` does **not** prove an OBS mixer
underrun. The actual mixer/encoded output must be measured independently.

The specific motivation for short lookahead is an OBS 32.2.2 hypothesis:
[`discard_audio`](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-audio.c#L289-L298)
can advance a partial source buffer's timestamp without consuming its samples.
An insufficient-buffer event could thereby move retained PCM by an OBS mixer
interval. The 1024-sample mixer interval is 21.333 ms at 48 kHz. That branch has
not been demonstrated in a runtime trace; an A/B timing improvement alone must
not be described as proof that the branch caused the original failures.

## Required validation and result labels

Keep these separate when reporting progress:

1. **Compilation:** module links against the target OBS installation.
2. **Module integration:** libOBS loads it; the shader compiles; all three source
   types produce media and shut down cleanly.
3. **Synthetic timing:** encoded irregular flash/click events retain their common
   timeline across staggered source creation, hide/show, stalls and reconnects.
4. **Audio controls:** independent mute, volume and routing work, including a
   rapid-toggle test with the intended microphone filters.
5. **Physical end-to-end:** requires its own [combined test evidence](physical-combined-validation.md);
   not proven by any of the above.

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
audio onset with the earlier due-only handoff. The new bounded-lead/sample-grid
handoff has subsequently passed the harness timing/onset checks in three fresh
40-ms baseline runs. Its failure/restart and microphone privacy matrix is still
under test; baseline success alone is not a production-readiness claim. See the
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

### Isolated boundary traces

The finite native recorder saves `.source0.csv` and `.source1.csv` alongside
its existing `.mix0.csv` and `.mix1.csv`. Source rows contain original
post-filter timestamps, approximately 1 ms stereo-downmixed RMS windows, mute
status and callback time. OBS 32.2.2 supplies the original timestamp to this
callback even though its mixer queues a separately adjusted copy. Therefore
source continuity alone does not establish mixer placement; compare matching
content on both timelines. Muted rows retain their activity measurement and
are explicitly labeled, not silently converted into measured silence.

The `.video.csv` trace records raw rendered-frame timestamps and cyan-marker
scores from a sparse 80x45 grid of the recorder's 640x360 NV12 output. Its
BT.709 limited-range conversion uses the physical helper's cyan predicates;
sampling is not identical to that helper's area scaling. This observes rendered
content before encoding, not capture-card latency or a physical display.

These test callbacks write only preallocated, bounded records; they perform no
file I/O or additional locking. Detachment fences in-flight writers before
exclusive-create CSV output. Invalid input or diagnostic-capacity exhaustion
fails the diagnostic. Keep these private activity/timestamp traces out of the
public repository. They are not an always-on production telemetry service.

- [OBS source implementation, 32.2.2](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-source.c)
- [OBS public API, 32.2.2](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs.h)
- [Linux monotonic clock implementation](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/util/platform-nix.c)

The two-second direct-audio timestamp threshold and small-discontinuity smoothing
are OBS implementation details, not stability guarantees for all future versions.
