# Network transport development validation — 2026-09-15

This is a bounded **desktop-audio transport** result, not a production release or
physical A/V synchronization result. The real OBS profile, production audio
senders, microphone, capture card, system clocks, firewall and startup tasks were
unchanged. Real desktop PCM was inspected in memory at the diagnostic receiver;
it was not recorded, played back, fed to IPC/OBS, or sent to a public service.
Private host-specific logs and media fixtures are excluded from this repository.

## Tested implementation

Windows MSVC 19.44, GStreamer 1.28.6 from a separate portable development SDK;
Linux GCC 13.3, GStreamer 1.24.2. Loopback preserved the existing eight-channel,
192 kHz float32 render configuration. Transport used explicit normalized stereo
mixing, quality-10 Kaiser conversion to 48 kHz S24BE, L24 RTP and capture-time
RTCP sender reports. The Linux monotonic clock was shared at application level;
no NTP/PTP or operating-system clock settings changed.

The SDK did not replace the existing Windows audio runtime. Linux development
headers were installed without upgrading the running GStreamer/audio stack.
These are tested combinations, not a claim about every supported SDK/device.

## A useful initial failure

The first two attempts connected to the shared clock but transmitted no audio:
the original rule rejected every packet timestamp later than current time. The
second attempt's diagnostics measured the raw WASAPI timestamp **14.399–30.320 ms
ahead** of local monotonic time, with the mapped timestamp ahead by the same
amount (less than a microsecond difference between the two diagnostic ranges).
This isolated the rejection to the endpoint timestamp relationship, not a QPC
unit or network-clock offset bug.

The corrected eligibility rule accepts at most 100 ms future or 100 ms old,
checks both before and after pipeline construction, and preserves the original
timestamp unchanged. It does not subtract the observed lead, rebase to arrival,
or pretend that lead is measured hardware latency. Content-time calibration
against unique A/V events remains necessary. [Clock caveats](network-clock.md)

## Three fresh starts

All three runs had non-silent received PCM, no post-start WASAPI discontinuities,
no timestamp-error flags, no sender drops/resets/queue overflows, and no receiver
stale/invalid/backward references or missing reference metadata after lock.
The receiver observed zero greater-than-2-ms reference-continuity differences
within each session. This is not a claim that every UDP packet was delivered.

| Overall sender deadline | Captured packets | Sender RTP packets | Timed receiver buffers | Untimed startup buffers |
| --- | ---: | ---: | ---: | ---: |
| 25 seconds | 2,351 | 7,052 | 6,999 | 52 |
| 15 seconds, fresh sender A | 1,351 | 4,052 | 4,002 | 50 |
| 15 seconds, fresh sender B | 1,348 | 4,043 | 3,942 | 100 |

The first receiver was also started fresh; the next receiver stayed open across
the two 15-second senders and created separate SSRC branches. Startup clock
acquisition is included in sender deadlines, so those are not exact PCM durations.
Untimed startup packets were explicitly counted, not given arrival timestamps.
The receiver is diagnostic-only and its negative reference ages (roughly
-35 to -10 ms in these runs) are not scheduled playback or negative latency.

Independent exact-rational reconstruction of the **last** sender RTP timestamp
from its last RTCP report agreed with that RTP buffer's PTS by -9.878, -0.653 and
-11.262 microseconds respectively, all within one 48 kHz sample (20.833 us).
The receiver's final reconstructed reference agreed with those sender PTS values
to the same sample-level precision. This verifies those transport timestamp
correspondences, not absolute hardware content timing or every packet in a run.
The checker deliberately refuses reset-aggregated summaries because they do not
contain enough SSRC evidence to compare generations safely.
[Checker and limitations](rtp-correspondence.md)

## Clock loss and reacquisition

A separate 30-second sender ran while the receiver paused its own clock replies
for five seconds, leaving RTP/RTCP reception active. The sender detected unhealthy
clock observations, dropped 756 capture packets (7.56 seconds of source frames),
reset once, required fresh observations, and resumed under a new SSRC. Linux saw
two sessions with 2,412 and 3,732 timed buffers, both with non-silent PCM and zero
stale/invalid/backward references or greater-than-2-ms within-session gaps.
The receiver confirmed both pause and resume actions executed.

This is **fail-visible recovery with a gap**, not seamless audio. The interruption
lasted longer than the injected outage because freshness detection and
reacquisition have their own timing. A later playout service must explicitly
handle missing media; this diagnostic does not conceal it or replay old samples.
Neither a process/machine reboot matrix nor arbitrary network loss was tested.
A separate start with the provider absent remained `waiting_clock`, with zero
captured packets and zero RTP output; it did not fall back to arrival timestamps.

## Conversion and regression checks

Actual offline conversions passed on both GStreamer runtimes, through F32 and
through S24BE packing/back to F32: all 19 generated segments, exact 91,200 output
frames, eight-channel routing, LFE exclusion, impulse placement, passband and
alias-rejection gates. These are generated signals, not recordings of the user.
See [conversion evidence and finite scope](audio-conversion.md).

Windows and Linux optional-network builds passed their five CTest entries.
Linux AddressSanitizer/UndefinedBehaviorSanitizer builds also passed those tests.
A further real-network run used the instrumented Linux receiver: 3,971 received
buffers, 3,867 timed and 104 startup-untimed, non-silent PCM, and no reported
sanitizer errors or reference failures. The 15-second sender had no drops,
resets or overflows; its independent final RTP/SR error was -5.310 us. This is
a bounded live callback/lifetime check, not sustained performance testing.
The dependency-free capture-window test exercises 12,032 cases; clock mapping
and health tests exercise 10,161 cases; existing timing tests exercise 2,021.
The Python suite passes 67 tests, including conversion-analysis and exact RTP
correspondence rejection tests. Unit/help tests themselves open no audio endpoint
or network listener. Runtime checks above are separate, explicit operations.

## Still open before a usable bridge

- Physical Elgato/V4L2 video timing, fixed content calibration and full 4K60 load.
- Adaptive audio-rate correction and thirty-minute drift/wow/flutter checks.
- Microphone capture, mute/filter privacy and separate production controls.
- Deadline-bounded retransmission, packet loss/reordering and authenticated trust.
- Network-to-IPC/OBS playout integration with nonuniform encoded A/V markers.
- Complete sender/receiver/OBS/machine restart matrix and hidden startup packaging.

Do not replace a functioning audio setup with these diagnostic executables.
