# Explicit video-only memory buffer

`avsync-video-bridge` connects physical V4L2 capture to the existing bounded IPC
ring. It is a finite development tool, **not a production synchronization service**.
It does not receive audio, launch OBS, change sources, install startup tasks,
negotiate capture formats, or claim physical A/V alignment.

The path is:

`four driver buffers -> two owned NV12 handoff slots -> delayed IPC ring -> optional in-memory verifier`

Capture copies each accepted frame before returning the driver buffer. The
ordinary worker publishes that independent copy with the original monotonic
capture time and `presentation = capture + configured delay`. Queue age and
dequeue time never replace the capture timestamp. No frame is manufactured to
cover a gap, and a damaged frame is counted and discarded.

## Explicit operation

Build with `AVSYNC_BUILD_V4L2=ON` and Linux IPC enabled; see
[building](building.md) and [capture limitations](v4l2-capture.md). Help does not
open a device. During a separately arranged maintenance window, release the
actual device owner and arrange to restore it even if capture fails:

```sh
./build-video/avsync-video-bridge --capture --device /dev/videoN \
  --runtime-dir "$XDG_RUNTIME_DIR/avsync-video-new-test" \
  --seconds 30 --delay-ms 2000 --verify-delivery
```

The device name is a placeholder. The runtime child must **not already exist**;
its parent must be an owned, private tmpfs directory. Do not use a recording
folder, network share, or a directory belonging to an existing bridge.
No process is automatically stopped or killed by this tool.

The current progressive single-planar NV12 format is required. The IPC/handoff
limit is 3840x2160, stricter than the metadata probe's 4096-pixel width limit.
The advertised frame interval only sizes capacity; it does not rewrite actual
frame times. Delay is 0..2000 ms, capture duration 1..120 seconds. Capacity and
dimensions must fit the existing 2 GiB IPC ceiling.

## Memory, thread ownership and failure behavior

At 3840x2160 NV12/60 fps and 2000 ms, the ring has 128 video slots and a
1,592,545,024-byte mapping (approximately 1.48 GiB). The two owned handoff slots,
four driver buffers, optional verification frame and process/library overhead
are additional memory. Audio slots remain unused; no silence is invented.

The writer reserves the entire file **before mapping or touching its header**,
then prefaults pages before capture starts. Failed reservation is a normal
initialization failure, not permission to proceed with a sparse multi-gigabyte
buffer. This reduces first-use faults; it does not pin RAM or guarantee hard
real-time scheduling. tmpfs can be swapped, so memory-backed does not mean
securely erased, non-pageable, or immune to later memory pressure.

The two-slot SPSC handoff allocates once, packs row padding, and does not block
or overwrite an unreleased frame. Full handoff slots cause counted frame drops.
The publication worker uses IPC try-locks; a busy lock causes a counted drop,
not a stalled capture callback. Frames older than 200 ms at publication are
discarded rather than used to build a stale worker backlog. Whole-frame copies
still cost CPU time; nonblocking locking is not a bounded execution-time proof.

SIGINT/SIGTERM request shutdown, with bounded delayed-frame drain on ordinary
completion. Kernel calls and the legacy writer destructor are not forcibly
interruptible; an independently suspended process holding the IPC mutex can
still delay destruction. This verifier uses its reader on the same worker as
publication and is not a general service watchdog.

Normal and handled-error exits close readers/writer and remove only this new
directory's known IPC files, checking directory identity. They never recursively
delete a supplied path. SIGKILL, a kernel fault or power loss cannot run cleanup;
a private runtime file may remain until explicit cleanup or runtime teardown.
Errors are visible and do not authorize deleting unrelated files to retry.

## What verification establishes

`--verify-delivery` copies the newest due IPC frame on the same worker. It checks
increasing sequence numbers, exact unchanged capture/presentation correspondence,
no future delivery, and a sampled pixel fingerprint against the publication
ledger. It reports skips, queue/lock pressure, delivery age and maximum observed
copy times. The fingerprint samples at most 4096 positions; it is neither a
full-frame cryptographic check nor a visual-quality assessment.

A late verifier may skip to a newer due frame, matching the existing video-reader
policy. Every skip remains visible and prevents a clean exit. Delivery age is
measured after copying into verifier memory; it is **not display, encoder or
stream latency**. `AVSYNC_VIDEO_READY` means initialization is complete, not that
capture has already delivered valid frames.

Exit 0 with `bounded_video_verified` requires at least two published/verified
frames, no observed capture or handoff losses, complete verification and cleanup.
Without verification, the clean label is `video_buffered_unverified`. Any
observed damaged frame, including startup frames, keeps the run
`degraded_or_failed` (exit 3). Argument errors exit 2. Do not filter startup
failures out of a report just to obtain a success status.

Raw V4L2 color metadata is available from the probe, but is not carried by the
current IPC layout. The existing OBS adapter assumes BT.709 limited-range SDR.
**Do not connect this real-video ring to that adapter until color interpretation
is explicitly validated.** This tool only checks byte/timestamp transport, not
matrix, range, transfer function, HDR, or rendered pixels.

See [physical video results](video-validation.md). Fixed HDMI/device content
calibration, audio-clock matching, a real OBS presentation test, startup priming,
sustained 4K60 load and restart recovery remain separate gates.
