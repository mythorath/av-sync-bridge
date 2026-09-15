# Linux synthetic IPC milestone

This is a synthetic, local-only milestone. It neither opens capture hardware nor
changes OBS, audio routing, startup tasks, the network, or microphone mute state.
Successful IPC tests are **not** proof of hardware timing or encoded OBS A/V sync.

## Contract

`include/avsync/ipc.hpp` is the API. The implementation is Linux-only and requires
the producer and consumer to share the same architecture and pthread ABI. Protocol
version 1 records a header size, mapping size, dimensions, capacities, region
offsets/strides and a random generation identifier; incompatible layouts are rejected.
All timestamps are signed CLOCK_MONOTONIC nanoseconds. Capture and scheduled
presentation timestamps remain separate. One generation owns one epoch.

Three fixed-capacity rings carry tightly packed NV12 video, interleaved float32
stereo desktop audio and float32 mono microphone audio. Audio is 48 kHz; the
synthetic producer uses 480-frame blocks. Every descriptor has generation, sequence,
capture time, presentation time, frame count and byte count. Sequence 0 is invalid.
Publishing enforces ordered per-stream capture/presentation timestamps and rejects
wrong payload sizes. The metadata is validated again before a reader copies a slot.

Each reader owns its cursors. Video returns only the newest unconsumed frame due
at `now`; audio returns the next unconsumed due block while discarding blocks older
than its age limit. Neither returns future media. Ring overwrite/stale drops are
counted. Reader buffers must be preallocated from the validated configuration.
The default permitted lateness is 200 ms for video and 100 ms for audio. These are
prototype safety bounds, not an agreed production synchronization tolerance.

## Boundedness and scheduling

Video is limited to even dimensions through 3840x2160 and 120 fps. Capacities are
bounded and complete mappings are capped at 2 GiB. The synthetic producer sizes
its rings from the requested delay plus a small margin, rather than reserving UHD
storage for a small test. It preserves the configured long delay in the ring;
the reader performs the due-time selection. This is not early submission of
multi-second-future audio into OBS.

Read methods use `pthread_mutex_trylock`: no waits, sleeping, file opening,
logging or memory allocation. A busy writer produces `busy`, not a blocked render
callback. Copies are still real memory copies (especially material at UHD), and
their cost must be benchmarked before production use. Reader construction,
reconnection and destruction perform I/O and belong on control/worker threads.
Reader instances are single-threaded; use one instance per consuming thread.

## Ownership, crash and restart behavior

The parent directory must be user-owned and not group/world writable. Mapping and
singleton-lock files must be user-owned regular files with mode 0600; symlinks are
rejected. A producer holds an exclusive nonblocking `flock` on the sidecar for its
lifetime. The sidecar must not be deleted while any producer may be running.

A producer initializes a fresh private file and publishes it by atomic rename.
It never truncates a file still mapped by old readers. Thus a restart leaves old
readers safely mapped to the old generation until explicit worker-thread
`reconnect()`. Clean shutdown marks that generation offline; a stopped heartbeat
expires after 2 seconds. A robust, process-shared mutex marks an epoch offline when
its lock owner dies, preventing consumption of a partly written slot. A new
producer/generation is required; it does not pretend an interrupted copy completed.

This is a trusted-same-user protocol, not a sandbox against malicious same-user
processes. A producer must not truncate a published mapping; a hostile owner can
always corrupt or remove their own files. Test builds can enable private
`AVSYNC_IPC_TEST_HOOKS` fault injection, which has no public API or CLI.

## Synthetic pattern and commands

`avsync-synthetic --help` describes options. The default is 640x360 NV12 at 60 fps,
two audio streams, 2000 ms delay, 10 seconds capture and a final delay-drain period.
`--duration 0` continues until interrupted. There are no network sockets.

Flashes occur near 1.00, 2.35, 4.10, 6.70, 10.15 and 14.80 seconds of each 20-second
cycle (rounded to video frames). Events carry binary identifiers in the top-left
pixels; the bottom row carries low frame-counter bits. Video flashes are ~100 ms;
corresponding 40 ms tone bursts use 660, 880, 1100, 1320, 1540 and 1760 Hz.
Both derive from the same capture epoch. At non-integral frame/sample ratios the
audio event is quantized by at most one sample. The mono test is half stereo level.
These are synthetic safe-level tones, not microphone capture.

Example in a pre-existing private test directory:

```sh
avsync-synthetic --path /private/test/media.ipc --duration 8 --delay-ms 2000
avsync-probe --path /private/test/media.ipc --duration 5 --verify --delay-ms 2000
```

Start the producer first and the probe while it is still running. Probe verification
checks receipt of all three streams, exact configured metadata delay, nonfuture
delivery and monotonically increasing sequences. It does not analyze tone/flash
content, test cross-machine clocks, or measure the encoded OBS result. Its reported
delivery lateness is local reader scheduling lateness, not measured A/V offset.

`ipc-tests` covers due-time selection, payload copying, invalid metadata/config,
private-file requirements, ring overwrite/stale dropping, heartbeat timeout,
singleton ownership, generation remapping and corrupt/unsupported headers. With
test hooks enabled it also deterministically kills a robust-mutex owner and tests
sequence exhaustion at UINT64_MAX. Production device reconnection and OBS loading
remain separate milestones.
