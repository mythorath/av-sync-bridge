# Timestamp-free nominal audio conversion

This milestone replaces the experimental sender's timestamp-sensitive
`audioresample` element with a stateful raw converter. It is **not** a deployed
audio bridge, adaptive drift controller, or physical A/V synchronization result.

## Contract

- One explicit stereo matrix, nominal conversion to 48 kHz float, then a separate
  deterministic S24BE quantizer and L24 RTP packetizer.
- No timestamps enter the nominal converter. Its input/output sample ledger
  advances continuously; the sender creates nominal timestamps with exact rational
  arithmetic, never repeated rounded per-packet increments.
- Original device-frame/mapped-QPC anchors stay separate in a sender-side tracker.
  Diagnostic rate estimates use those originals, not nominal output timestamps.
  **The originals are not on the wire yet.** RTCP/reference timestamps cannot
  qualify receiver ASRC or establish content-time synchronization.
- Input is bounded to half a second per call. Float PCM must be finite, spans
  must not overlap, and invalid input is rejected before state changes. Converter
  setup/recovery may allocate; no allocator-tracing or hard real-time claim is made.
  Accepted rates are 8–384 kHz with `48000/gcd(input_rate,48000) <= 640`;
  ordinary audio rates qualify, but pathological coprime rates are rejected.
- Filter lookahead delays availability, not the sample-grid origin. Normal stop,
  clock faults and capture gaps discard retained history. The separately exposed
  `finish()` operation is for offline/clean EOS only and truncates zero lookahead
  to exactly the real input's nominal duration.
- A fresh sender generation reconstructs conversion state and chooses a new
  SSRC. The bounded sender checks original capture freshness again after
  conversion/allocation. This is not yet a receiver oldest-sample playout deadline.

## Independent checks

`avsync-nominal-audio-tests` exercises short and irregular input partitions,
integer/noninteger rate ratios, reset reproducibility, format/size validation,
silence, nonfinite samples, impulses and clean EOS.

`avsync-nominal-audio-probe --offline` compares identical generated PCM sent in
fixed and irregular chunks. Its checks cover complete sample counts, all-sample
partition agreement, an unshifted analytic reference, isolated channels,
nonuniform Gaussian markers, fractional-position impulses and silence. No
cross-correlation or fitted phase is used to move the expected signal. A deliberate
one-output-sample shift must fail. Reported fitted tone phase is diagnostic only.

The optional `--ppm` field describes an independent analytic original-clock
duration. It is not passed into this timestamp-free converter and does not
simulate network anchors or adaptive correction. The long generated cases cover
90 seconds of PCM, not 90 seconds of real hardware or a sustained-load test.

The Python suite validates counts and duration arithmetic independently of the
executable's pass flag. Its quick matrix runs in optional network CTest builds;
the full matrix is selected explicitly with `--long`.

## Failures retained during implementation

The strict signal tests initially failed on both tested runtimes. Three
adjustments were necessary; none was hidden by shifting the reference or
relaxing the error gates:

1. Reconstruct the raw converter on reset. The tested backend's reset cleared
   history without restoring fractional resampler phase; setup warmup followed
   by reset therefore contaminated the nominal origin.
2. Use full, directly calculated filter phases rather than the default
   interpolated coefficient path. The first implementation had an approximately
   1/64-output-sample tone phase offset at 192 kHz and 0.034-output-sample offset
   at 44.1 kHz, even after fresh construction.
3. Supply explicitly initialized PCM for offline EOS lookahead. The nullable
   input path produced chunk-dependent end samples in the tested multichannel
   conversion. Its unpack implementation passes a sample count into a
   byte-length silence helper. Explicit native-format padding avoids that path.

These observations concern this raw converter integration, **not a diagnosis of
an unrelated production audio incident**. Relevant primary implementations:
[resampler](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-resampler.c),
[converter](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-converter.c).

## Generated results — 2026-09-15

Windows MSVC 19.44 / GStreamer 1.28.6 and Linux GCC 13.3 / GStreamer 1.24.2 each
passed all **59** extended cases: 57 positive cases and two correctly rejected
one-sample-shift controls. All positive fixed/irregular partitions were exactly
equal on their respective runtime, including the EOS tail. The worst positive
analytic RMS residual was -140.54 dBFS on Windows and -140.45 dBFS on Linux.
This compares generated PCM, not inter-platform bit identity or real audio quality.

Both 90-second, 192 kHz/eight-channel marker cases produced exactly 4,320,000
stereo output frames after clean EOS, with all six markers at their expected
sample locations. Their separate +/-500 ppm analytic clock durations intentionally
diverged from nominal duration; no adaptive correction was performed. The
90-second 44.1 kHz tone case also passed its unshifted analytic/count checks.

Windows optional-network CTest passed 12 entries; Linux combined network/ASRC
CTest passed 15 in both optimized and AddressSanitizer/UndefinedBehaviorSanitizer
builds. The converter unit executable passed 3,001,425 checks on Windows.
The whole generated matrix was run on optimized builds; sanitizer coverage
includes the quick matrix and converter unit tests, not all 59 long-suite cases.

## Bounded live transport results

The same Windows/Linux runtimes were tested with real desktop loopback from an
existing eight-channel, 192 kHz float32 endpoint, sent over a dedicated LAN to
the in-memory diagnostic receiver. Normal OBS, audio services, microphone mute,
startup, firewall and OS clock settings were unchanged. Audio was not recorded,
played by the receiver, or submitted to OBS.

| Overall sender deadline | Capture packets | Converted/transmitted/received stereo frames | RTP packets received | Maximum conversion call |
| --- | ---: | ---: | ---: | ---: |
| 25 seconds | 2,329 | 1,117,792 | 6,986 | 0.393 ms |
| 100 seconds | 9,803 | 4,705,312 | 29,408 | 0.852 ms |

Both runs had nonzero received PCM, zero post-start capture discontinuities,
timestamp errors, sender resets, drops, queue overflows, nominal RTP frame/PTS
steps or malformed payloads. Receiver frame and packet totals matched sender
output totals, with no invalid/stale/backward references or greater-than-2-ms
reference gaps. Startup references remained explicitly unqualified for 110 and
99 buffers respectively; no reference metadata disappeared after lock.

The longer run contains approximately 98 seconds of actual capture; the overall
deadline also includes clock acquisition. Its original-anchor tracker made 97
one-second estimates, none outside the configured +/-500 ppm range. Its final
estimate was approximately -7.22 ppm, a diagnostic of this run, not a permanent
calibration or synchronization-error bound. Converter lookahead was 512 input
frames (2.667 ms); 128 output frames remained buffered and were intentionally
discarded at each finite stop, rather than fabricated or flushed into a new epoch.

These maxima are observed worker-call timings, not guarantees under gaming load.
Matching frame counts do not independently verify PCM bit identity, perceptual
quality, capture-clock uncertainty, or physical A/V alignment. Original capture
anchors still need their own wire contract before receiver drift control.

A separate 40-second sender run paused **only the diagnostic provider** for five
seconds. The sender performed one generation reset, rejected 758 capture packets
while clock health/reacquisition was unqualified, then resumed with a fresh SSRC.
Both receiver sessions obtained valid references and nonzero PCM, with no detected
within-session timing steps. This is deliberate interruption/recovery, not
seamless audio: 9,214 packets were counted at the sender output but 9,213 at the
receiver (192 frames / 4 ms unmatched). The missing packet's precise location and
cause were not isolated; the pre-UDP sender counter and drop-on-reset shutdown
do not prove delivery. Packet recovery and boundary-loss accounting remain open.
The test processes exited, their diagnostic listeners closed, and normal OBS,
both production audio receivers and the microphone mute state were rechecked.

## Remaining live-system gates

Versioned original-anchor transport, bounded receiver association of metadata
with PCM, adaptive correction, clock-uncertainty/load testing, microphone privacy,
physical content-time calibration and combined real-video/audio OBS delivery
remain separate gates. A non-silent diagnostic receiver is not an audible-quality
or physical A/V test. The existing production setup is not migrated by this code.
The results above describe the nominal-conversion milestone. The later
[original-anchor transport milestone](audio-anchor-transport-validation.md) adds
the experimental [wire contract](audio-anchor-wire-draft.md), explicit finite
clock-epoch agreement and exact calibration provenance. It remains separate
from adaptive correction and physical A/V validation.
