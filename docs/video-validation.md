# Physical video development validation — 2026-09-15

This is **physical video capture plus an in-memory delayed handoff**, not physical
A/V synchronization or OBS output validation. Tests used an Elgato 4K X on Linux
kernel 7.0.0-31-generic, GCC 13.3, existing 3840x2160 progressive NV12 at an
advertised 60 fps, stride 3840 and 12,441,600-byte frames. Driver/module settings
and the capture format were not changed.

Normal OBS was confirmed idle, stopped for exclusive device ownership, and
reopened after each maintenance window. A consistent backup was taken while OBS
was stopped. The normal canvas/output, source settings, audio routes and mic mute
were preserved. No production audio sender, system clock, startup task, network
route, microphone or camera scene was migrated to this prototype.

Video pixels existed in private memory-backed IPC during the buffer tests, then
the known temporary runtime files were removed. No desktop video was recorded or
published. Private host-specific logs are excluded; only aggregate measurements
are reported here.

## Metadata-only capture: do not hide startup damage

| Capture deadline | Dequeued | Accepted | Driver-error frames | Missing accepted sequences | Accepted timestamp cadence |
| --- | ---: | ---: | ---: | ---: | ---: |
| 30 seconds | 1,753 | 1,747 | 6 | 5 | 59.829 fps |
| 60 seconds | 3,558 | 3,553 | 5 | 5 | 59.916 fps |

These deadlines include stream startup, not exactly that many seconds of valid
frames. Both runs completed capture but returned degraded status because of the
errors. Missing accepted sequences overlap discarded driver-error frames; do not
add those counts together as independent losses.

The instrumented 60-second run placed all five error frames at sequences 3..7,
750.744..817.406 ms after run start. Its first accepted frame arrived at
700.751 ms; the single five-sequence gap ended at 834.087 ms. No further driver
errors or accepted-sequence gaps occurred before the last frame at 59,983.414 ms.
This supports **startup-localized errors in that run**, not a guarantee that
future errors can only occur at startup. No warmup exclusion was applied to
the counters or exit status.

The 60-second capture's accepted timestamp intervals ranged from 16.625 to
100.000 ms, including that gap. Timestamp-to-dequeue age ranged from 14.526 to
15.665 ms. There were no invalid payloads/timestamps, flag changes, chronology,
I/O, callback or cleanup errors. Startup polling timeouts remain counted.

The device reported monotonic/SOE flags. The UVC module used `CLOCK_MONOTONIC`
with hardware timestamping disabled. Consequently, those flags must not be
described as independently verified HDMI event/exposure timestamps. See the
[driver source review](video-timing.md). The approximately 15 ms dequeue age is
**not** an end-to-end capture latency calibration.

## First physical buffer attempt

A 30-second, 2000 ms video-only run captured 1,740 accepted frames, published
1,739, and verified 1,737. Six driver-error frames and a five-sequence accepted
gap remained visible. One handoff-full drop and two newest-due reader skips
prevented a clean result. Every delivered frame passed its original timestamp,
exact configured delay and sampled pixel correspondence checks.

Measured verifier delivery age was 2001.090..2020.140 ms; maximum capture-copy,
publication and read-call durations were 2.219, 15.831 and 5.913 ms respectively.
These are observations from that short run, not latency guarantees.

This attempt reserved its tmpfs storage after constructing the writer. Review
found that the sparse header could already fault on a full filesystem before
reservation. The revised writer reserves **before any mapped access** and
prefaults pages before capture. The change addresses that initialization failure
and reduces a source of first-use page faults; one uncontrolled comparison
cannot attribute every observed scheduling drop to allocation.

## Reserved/prefaulted buffer checks

The next 60-second run delivered and verified **all 3,558 accepted frames** with
zero handoff-full, publication-busy, stale-publication or newest-due reader skips.
Five driver-error frames still occurred during startup, with the last at
729.570 ms; the single five-sequence gap ended at 746.245 ms. The run therefore
correctly remained degraded overall rather than claiming loss-free capture.

Verifier delivery age was 2001.100..2009.480 ms. Maximum observed capture-copy,
publication and read-call durations were 4.993, 4.654 and 4.781 ms. Every delivered
frame retained the exact 2000 ms timestamp offset and passed sampled pixel
correspondence. This is one finite no-additional-drop handoff result, not a
promise about different load, devices, or real OBS presentation.

For that complete process, including reservation/startup/drain, the resource
report recorded 64.74 seconds wall time, 15.92 seconds user CPU and 4.36 seconds
system CPU (31% of one CPU), zero major page faults and zero reported swaps.
Maximum RSS was 3,198,632 KiB. The verifier maps the same shared file a second
time, so that RSS is **not a measurement of unique physical memory used by the
ring**. The fixed IPC mapping remained 1,592,545,024 bytes; application copies and
driver memory are additional. No long-run memory-pressure or GPU test is implied.

Two further fresh starts used the same unchanged delay and format:

| Build / capture deadline | Accepted = published = verified | Startup driver errors | Last error after run start | Verifier delivery age |
| --- | ---: | ---: | ---: | ---: |
| RelWithDebInfo / 30 seconds | 1,756 | 5 | 750.650 ms | 2001.070..2009.370 ms |
| ASan + UBSan Debug / 15 seconds | 850 | 5 | 878.464 ms | 2001.250..2011.300 ms |

Both had zero handoff-full, publication-busy, stale-publication or reader skips;
both retained a single five-sequence capture gap and correctly returned degraded
status. All sampled payload/timestamp checks passed. The instrumented process
reported no address, leak or undefined-behavior sanitizer failures. That finite
check is not ThreadSanitizer or an exhaustive race/fault test.

Across the three reserved/prefaulted runs, **6,164 accepted frames were published
and verified with no additional handoff/reader losses**. All three temporary
video runtime directories were confirmed removed, and the ordinary OBS session
was reopened with its existing settings. These are process/capture fresh starts,
not a computer reboot or long-run performance matrix.

## Regression coverage

Windows MSVC Release passed its five CTest entries. Linux RelWithDebInfo and
ASan/UBSan Debug each passed seven entries, including CLI help (no capture),
portable timing/handoff policies and IPC behavior. The handoff tests include
40,079 checks plus a 50,000-frame two-thread fixture; timing validation includes
32,283 checks. All checks remain active in Release builds. The Python suite
passed 67 tests.

IPC tests cover nonblocking publication under a held mutex, dead-owner rejection,
reserved video/audio compatibility, ring wrap, failure to reserve, preservation
of a previous published mapping, temporary-file cleanup and retry. The actual
reservation syscall is forced to fail by a five-second-bounded child with a
zero-byte file-size limit; no host filesystem is filled. Existing default writer
allocation and IPC layout remain compatible. This fault fixture is not an
exhaustive simulation of kernel, filesystem or process failure.

After rebuilding the existing native OBS adapter against the IPC changes, the
isolated synthetic baseline and mute fixtures both passed. The baseline matched
all six nonuniform events per audio track within its one-frame median/two-frame
maximum encoded timing gates, and both raw-mixer timing checks passed their
2 ms bound. The mute case suppressed exactly its designated microphone event;
all remaining encoded marker offsets were approximately -1.667..-1.000 ms, and
its desktop raw-mixer check passed. These fixtures use generated media on an
isolated display, not the physical Elgato, real microphone or production profile.

## Limits and next gates

- The verifier checks sampled byte and timestamp correspondence, not rendered
  color, full-frame hashes, encoder throughput, screen latency or physical A/V sync.
- Corrupt startup frames still need an explicit readiness/recovery policy. Do
  not silently relabel a run with capture errors as clean.
- Current IPC lacks physical color metadata; the OBS adapter assumes BT.709
  limited-range SDR. Color/range/transfer interpretation must be validated first.
- Original audio-device clock observations must survive conversion before
  adaptive rate correction; see the [ASRC design gate](asrc-design.md).
- Physical nonuniform A/V markers, combined audio/video OBS delivery, sustained
  loaded 4K60 and the complete restart/mute matrix remain unproven.

The [video buffer contract](video-buffer.md) documents bounds, explicit operation,
cleanup and status semantics. This checkpoint is not a production release.
