# Bounded Linux video capture

`avsync-v4l2-probe` is a Linux-only, explicitly started metadata diagnostic. It
uses the current progressive, single-planar NV12 format; it does not negotiate a
different resolution or frame rate. Help and invalid arguments never open a
device. There is no enumeration, automatic device selection, recording, pixel
inspection, display, IPC output or network output in this executable.

```sh
avsync-v4l2-probe --help
# Only after arranging exclusive device ownership during a maintenance window:
avsync-v4l2-probe --device /dev/videoN --capture --seconds 10
```

`/dev/videoN` is a placeholder, not an auto-selected input. Stop the actual owner
using its normal control mechanism first, and restore it afterwards. The probe
never kills a competing process. Buffer allocation/stream start can report busy;
successful `open()` alone is not evidence of exclusive ownership. Hardware and
drivers differ in whether simultaneous streams are possible.

## Bounds and ownership

- A run is single-use, accepts 1..120 seconds, polls in at most 100 ms slices and
  responds to SIGINT/SIGTERM. A pending caller cancellation is checked before
  buffer allocation or stream activation, including after slow downstream-ring
  setup. Kernel driver calls and a caller-supplied callback
  cannot be forcibly interrupted by this library's deadline.
- Exactly four MMAP buffers are requested and required. Each mapping is at most
  32 MiB, dimensions at most 4096x2160, even width/height/stride, and stride at
  least width. The allocation must hold both NV12 planes including row padding.
  This is a deliberately limited first capture adapter, not universal V4L2.
- The library exposes a borrowed `V4l2FrameView` only during its synchronous
  callback. A downstream consumer must copy into bounded owned storage before
  returning. No capture buffer may remain borrowed after requeue. Callbacks must
  not wait for IPC, encoding, output deadlines or another thread.
- Error-marked frames are counted and discarded, then their buffer is requeued.
  Malformed payloads, unknown timestamps, time/sequence regressions and changed
  timestamp flags fail closed. An ioctl error does not cause a guessed buffer
  index to be requeued.
- Normal exit and exceptions stop streaming, unmap buffers and free the buffer
  pool. The descriptor is then closed by object destruction. No S_FMT, S_PARM,
  input or control writes occur, so there are no changed settings to restore.

## Time and interpretation

Only explicit `V4L2_BUF_FLAG_TIMESTAMP_MONOTONIC` timestamps with a recognized
timestamp-source flag are accepted. Canonical timeval fields are checked before
conversion to signed 64-bit nanoseconds. The original capture timestamp is kept
separate from dequeue time; neither wall time nor arrival time is a fallback.
Timestamp age must be within two seconds old and one millisecond future relative
to `CLOCK_MONOTONIC`. This is a sanity/staleness gate, not a content calibration.

The probe preserves and reports whether the driver advertises start of exposure
or end of frame. Those flags alone **do not prove the time of the visible HDMI
event**, sensor exposure, or a fixed capture-card latency. Driver timestamp
generation, USB buffering and device behavior still need direct measurement; see
[video timestamp research](video-timing.md).

Sequence progression uses unsigned 32-bit modular distance, including wrap.
Repeated, backward and ambiguous half-range jumps fail closed; forward gaps are
counted. Driver sequence gaps do not reveal every upstream HDMI/device loss and
sequence continuity does not prove that pictures are visually distinct. A new
capture object is a new generation, never a hidden rebase of the old generation.

The summary omits device paths, names, serial numbers and image content. Format
color fields remain raw V4L2 values, including unspecified defaults; this adapter
does not invent a color interpretation for OBS. Extended color fields are read
only when capability and returned magic indicate validity; otherwise zeroes are
accompanied by `extended_color_fields_valid: false`. Reported cadence is based on
accepted capture timestamps, not the requested/advertised frame rate.

To distinguish startup faults from ongoing faults, diagnostics retain metadata
for at most the first 16 driver-error buffers: sequence, flags, payload size,
dequeue time relative to run start, and capture time relative to run start only
when its clock/source/age checks pass (otherwise JSON `null`). The total error
count, omitted event count and last error time remain available after this fixed
array fills. First dequeued/accepted and last accepted times include allocation
and stream-start overhead. The maximum accepted timestamp interval's ending time
locates its gap within the run. These observations never read image bytes or
rebase presentation timing; startup errors are still errors, not hidden from the
exit status.

Exit 0 means at least two accepted frames and no observed gaps, errors or manual
interruption in this bounded run. Exit 3 denotes incomplete/degraded capture;
exit 2 denotes argument or opening/format-query failure. None proves A/V sync,
visual quality, sustained 4K60 performance or restart resilience.

## Primary references

- [Linux V4L2 buffer timestamps and flags](https://docs.kernel.org/userspace-api/media/v4l/buffer.html)
- [Buffer allocation and ownership](https://docs.kernel.org/userspace-api/media/v4l/vidioc-reqbufs.html)
- [Queue/dequeue behavior and errors](https://docs.kernel.org/userspace-api/media/v4l/vidioc-qbuf.html)
- [Current format query](https://docs.kernel.org/userspace-api/media/v4l/vidioc-g-fmt.html)
- [V4L2 polling](https://docs.kernel.org/userspace-api/media/v4l/func-poll.html)
