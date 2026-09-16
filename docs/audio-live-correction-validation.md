<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Desktop correction: quality, resources and isolated live boundary

Checkpoint: 2026-09-15. **Not a production audio bridge or physical A/V sync pass.**
The new explicit `--correct-desktop` receiver mode decodes ordered L24 PCM and
its original anchor from the same RTP packet, gives a bounded owned copy to the
correction worker, and measures/discards its output in memory. It never feeds
OBS, IPC, a playback device, a file or the microphone. Ordinary services,
routes, mute state, capture devices and startup tasks are unchanged.

## Changing-ratio sound quality

`avsync-asrc-quality-probe` tests the existing BEST_QUALITY stereo backend at
fixed 480-frame output capacity and 2,048-frame input availability. The command
traverses -499 to +499 ppm and back in 0.99 ppm steps per output quantum. All
unconsumed input is retained by exact sample index; no EOS padding is used.
The first 250 ms is excluded from continuous-signal residual/gain comparisons;
whole-run peaks and isolation still include startup. End-of-run lookahead is
discarded, not presented as a tested finite-segment tail.

The reference is the analytic signal evaluated at the previously
[independently waveform-checked phase model](audio-phase-validation.md)'s
fractional source position. This measures DSP fidelity **conditional on that
model**, not independent capture-clock accuracy. The original nonuniform-marker
fixtures remain required. One second of nominal periodic input is prepared
before pacing; the fractional output reference remains analytic. Isolated
residuals exclude the silent channel rather than diluting the error with zeros.

The 24-second-per-signal matrix passed both isolated channels at 1/10/18 kHz,
distinct left/right multitone signals, DC, silence and three nonuniform impulses.
Worst guarded continuous RMS residual was -146.989 dBFS; maximum guarded sample
residual was -135.404 dBFS. Gain error was below 0.000002 dB. Isolated-channel
leakage and silence were exactly zero in these fixtures (-300 is the reporting
floor, not measured analog dynamic range). Impulse peak position error was
0.480840 source frames. Impulses are tested for peak location, finite/headroom
and isolation, **not** against a continuous-tone residual criterion.

The stereo multitone's measured input/output peaks were 0.398400/0.399641.
Maximum whole-matrix output peak was 0.519355, including finite-start ringing
on a 0.5-amplitude tone. No limiter or normalization was added. Existing gates
remain RMS <= -80 dBFS, local error <= -50 dBFS, gain within 0.2 dB, isolated
leak <= -100 dBFS and whole-run peak <= full scale. Deliberate 2% output
attenuation correctly fails the residual gate (-43.010 dBFS) even though its
gain error alone is within 0.2 dB.

## CPU: retain the failures, specify the tested condition

Host: first-generation eight-core Ryzen 7, eight online CPUs, Linux GCC 13.3
RelWithDebInfo, distribution libsamplerate 0.2.2. OBS, desktop display, browser
sources and remote desktop remained active. Neither this host nor CI is a
controlled hard-real-time environment. Each backend call includes wrapper
validation, with **exactly one 480-frame/10-ms output quantum**. Percentiles are
upper histogram bounds in 10 us bins, not burst time divided by call count.

| Measurement | P99 call upper bound | Maximum | Calls over 10 ms | Pacing wakes >10 ms late |
|---|---:|---:|---:|---:|
| Initial 24-second accelerated matrix, worst case | 1.940 ms | 6.497 ms | 0 | Not paced |
| Initial 60-second paced multitone harness | 3.200 ms | 14.872 ms | 1 | 5,590 |
| Fixed input-generation overhead, 60-second paced | 2.650 ms | 6.506 ms | 0 | 0 |
| Same fixed harness, test process on one CPU, 60-second paced | 1.560 ms | 5.340 ms | 0 | 0 |

The first paced harness repeatedly generated thousands of long-double sine
values for already supplied lookahead, overloading its own pacing. Preparing
periodic input beforehand fixed those missed wakes; it did **not** by itself
pass the proposed p99-under-2-ms CPU gate. A subsequent process-local affinity
trial passed that gate without changing the backend, quality setting, OS
governor, service priorities or system-wide scheduling. The live trial uses
that explicit isolated affinity condition. This is not proof that affinity
always fixes performance or that an unpinned production configuration passed.
The fixed paced output also passes quality: -159.076 dBFS RMS residual,
0.399809 maximum output peak. A representative 30-minute loaded soak is open.

## Allocation, bounded memory and startup failure

The opt-in **test-only Linux/glibc** interposer observes malloc/calloc/realloc/free
on the explicitly enabled thread, including calls from the actual shared DSP
library. The probe first observes construction, then 600 simulated seconds of
varied-packet worker operation with rate changes, followed by four resets and
eight-second re-priming runs. Interposer counters use a fixed 512-entry table;
overflow is a failure. Exception/loader warmup and logging are outside scopes.

- Five constructor allocations; 1,136,388 total requested heap bytes, including
  the heap-allocated worker object and DSP state (about 1.084 MiB).
- Zero additional allocations during steady processing or the four resets.
- Identical live/peak requested bytes after the run; zero tracked allocations
  left after destruction.
- Each of the five constructor allocations is failed exactly once in separate
  controls. All five produce an explicit exception and clean up acquired state;
  no silent unity-conversion fallback.

These are **requested heap bytes**, not RSS, allocator arena overhead, stack
usage or total GStreamer/OBS memory. This narrow audit covers the allocator APIs
used by the audited worker/library, not arbitrary mmap/aligned allocations or
other threads. Never preload this interposer into OBS or production services;
it is incompatible with the separate sanitizer configuration. The ingress
diagnostic has an additional fixed 64-packet value-owned queue and at most eight
session summaries; this worker-only allocation figure does not include it.

## Combined generated path

`avsync-audio-chain-tests` feeds generated eight-channel 192 kHz float input
through the actual nominal converter, encodes/decodes RTP/L24 with the original
anchor extension, then runs ASRC and checks **all five** nonuniform output
markers against the independent continuous-time source. Packet sizes vary from
1 to 180 frames. The RTP counter crosses its 32-bit timestamp wrap. This is an
in-memory chain, not a socket, jitterbuffer or physical capture test.

| Injected rate | RTP packets | Nominal wire frames | Corrected frames after priming | Worst marker error |
|---|---:|---:|---:|---:|
| 0 ppm | 9,597 | 575,872 | 429,120 | 0.000001 ms |
| +499 ppm | 9,597 | 575,872 | 426,720 | 0.005523 ms |
| -499 ppm | 9,597 | 575,872 | 429,120 | 0.002077 ms |

All markers pass the one-output-sample gate. Different corrected counts reflect
the independently established priming origin and bounded undrained tails, not
an assertion of exact finite-segment duration. The diagnostic adapter separately
passes 29,166 checks for signed L24 extremes, owned-buffer isolation, generation
replacement/retirement, missing reports, clock loss, stale input, missing frames,
full queues, future arrivals and backward caller time. The new chain, adapter
and quality tests also pass ASan/UBSan; system DSP/GStreamer libraries themselves
are not sanitizer-instrumented.

## First actual desktop correction trial

The receiver inspected 1,907,872 ordered stereo frames from the real Windows
eight-channel 192 kHz loopback. It discarded 145,012 priming frames and produced
and inspected 1,759,680 corrected frames (36.66 seconds). Output was nonzero:
peak 0.256195, RMS 0.0225982. That is level evidence, not a listening test.

There were 39 valid original-clock rate windows, no out-of-range windows, no
phase/metadata faults, and no reported jitter late/capacity drops. The final
command and target agreed at -6.88995 ppm. Maximum modeled disagreement against
the supplied original anchors was 38,308 ns; **this is not independently measured
physical A/V error**. Maximum output capture age was 76.834 ms, queue peak eight
packets, input peak 3,007 frames, output peak 2,400 frames and maximum five DSP
quanta per dispatch, all within their bounds.

This finite receiver intentionally stopped while the sender remained active, so
its last three ingress packets had not reached the ordered callback at teardown.
The sender subsequently lost this test clock provider and stopped sending; its
later capture drops belong to that teardown interval. Whole-process sender and
receiver counts therefore must **not** be presented as an exact lossless match.
The receiver report distinguishes ingress, ordered input, priming and corrected
output. No production source, service or microphone was switched.

### Recovery check: a failed verdict was found and fixed

The first five-second provider-pause run recovered, but then suffered an extra
unplanned loss of sender clock qualification. It had **three** media generations,
not the two expected by the fixture. Its original verdict was too permissive:
it allowed any retired stale/health-faulted session whenever pause mode was set.
The test now requires exactly two generations and retains the first fault's
local time. The first must be a health fault within 100 ms after the actual
provider-pause edge; the final session must be running. Unit controls reject a
third recovered session, stale instead of health, incorrect fault time and a
faulted final session. The earlier run is a **failure**, regardless of its old
`correction_diagnostic_pass=true` output.

The sender previously reset its health monitor before reporting the reason for
losing qualification. It now retains up to eight loss-edge snapshots (reason,
elapsed time, observation age, RTT, calibration slope and mapped-packet count).
No capture PCM or endpoint identity is added. The specific cause of that first
extra outage cannot be recovered retrospectively from its old report.

A repeat under the stricter verdict passed with exactly the requested two
generations: 1,251,360 and 2,796,000 inspected corrected frames. The first stopped
at the provider-pause edge; the second re-primed and stayed running. Maximum
modeled phase error was 42,266 ns, packet-queue peak eight, output peak queue
3,360 frames and maximum seven DSP quanta per dispatch. The sender's new loss
records show stale observations at the deliberate pause and final provider
teardown, not an intervening third outage. This is finite interruption/recovery,
not seamless output; priming and unqualified capture are intentionally discarded.

The receiver ingressed and validated all 28,834 RTP packets / 4,613,504 nominal
frames it observed before shutdown. Sender totals still extend past receiver
teardown and must not be equated to those partial-window totals.

### Polling robustness follow-up

The sender now explicitly caps the **underlying** network clock's adaptive
polling/retry timeout at 250 ms; the minimum update interval remains 100 ms.
The two-second observation freshness limit, RTT, discontinuity and rate gates
are unchanged. Setting the inherited timeout on the wrapper would not configure
the clock doing the polling. Upstream's default internal timeout is one second;
accepted-observation filtering can leave fewer useful observations inside the
freshness window. This change adds polling opportunities rather than accepting
stale timestamps. It is not proof of the earlier outage's exact cause or an
absolute clock-error bound. The sender reports the actual read-back timeout.
[Pinned upstream polling implementation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gstreamer/libs/gst/net/gstnetclientclock.c#L535-L598),
[retry timer](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gstreamer/libs/gst/net/gstnetclientclock.c#L667-L685).

With that explicit timeout, the next no-pause 110-second receiver run passed as
one uninterrupted generation. All 32,594 ingressed RTP packets / 5,215,072
nominal frames reached ordered validation. The worker inspected 4,873,920
corrected frames (101.54 seconds), with no phase/metadata faults, jitter drops
or unplanned clock loss during the receiver window. The maximum predicted
anchor disagreement was 54,402 ns and maximum output age 77.193 ms. The sender
reported 316 clock observations before its only loss, at 111.594 seconds after
the receiver/provider had intentionally stopped. Its 42 later discarded capture
packets belong to that teardown, not the tested running window. Startup priming
discarded 338,932 frames (about seven seconds); acquisition latency is not yet
optimized and is explicitly not gapless recovery.

A further 105-second run with the 250 ms timeout and a five-second provider
pause also passed the strict two-generation verdict. It inspected 689,760 and
2,834,880 corrected frames, with maximum predicted phase error 41,450 ns, no
jitter drops or invalid records, and no unplanned sender clock loss during the
receiver window. All 29,911 ingressed packets / 4,785,824 nominal frames reached
ordered validation. The two logged sender losses coincide with the deliberate
pause and final teardown. **Initial correction acquisition took about 19.24
seconds in this run**, and about 5.04 seconds after recovery. The three-window
20 ppm acquisition-spread rule was not relaxed to hide that delay. Variable
startup qualification remains an explicit production/usability gate; these
short successes do not establish unattended or long-term reliability.

## Regression and Windows test-launch correction

The final local suites passed: 30 Linux optimized CTest entries (including the
separate allocation audit), 29 Linux Debug ASan/UBSan entries, eight Windows
core entries, 15 Windows network entries, and 76 Python helper tests on each
host. Sanitized system DSP/GStreamer dependencies are not implied. The full
Debug sanitizer suite took 289.75 seconds on the test host.

A final Windows rerun initially omitted the SDK runtime from its shell PATH.
Seven network entries failed before useful execution, and repeated child
launches produced missing-core/audio-DLL system dialogs. That is a test-launch
failure, not a measured audio failure. The offending run was stopped; no
production sender or runtime installation was changed. Configuration now checks
the matching SDK's core/audio DLLs and gives every Windows network CTest entry
a test-local runtime PATH, inherited by Python fixture children. All 15 entries
then passed from a fresh shell without manually adding the SDK to PATH. Direct
standalone diagnostic launches still need the documented runtime environment.

## Open boundaries

Still required: loaded repeatability and full source-clock uncertainty analysis;
presentation-buffer/IPC handoff of corrected PCM; simultaneous physical video
and audio scheduling; unique-event content-time calibration and encoded OBS
measurements; loss/reordering recovery; automatic restart handshake; microphone
capture/filter/mute privacy; and the representative 30-minute/restart matrix.
This live **inspect-and-discard** experiment is not permission for automatic
startup migration. See [receiver instructions](network-receiver.md) and
[release gates](roadmap.md).
