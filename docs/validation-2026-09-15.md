# Initial validation: 2026-09-15

Historical baseline report. The failures below are preserved; the subsequent
[handoff revision and recovery tests](handoff-validation.md) have newer results.

**Status: usable development foundation, not a production A/V bridge.**
The portable core and Linux IPC tests pass. The OBS module records generated
media successfully. Repeated encoded-output tests do **not** yet meet the
repeatability gate. No real capture or network-audio synchronization is claimed.

## Scope and environment

- Portable core: Windows MSVC 19.44 Release; Linux GCC 13.3.
- Linux integration: Ubuntu 24.04, CMake 3.28.3, libOBS 32.2.2.
- Synthetic IPC: 640x360 NV12 at 60 fps, stereo and mono float PCM at 48 kHz,
  480-sample blocks, two-second presentation delay.
- Standalone libOBS on Xvfb; x264 ultrafast, CRF 16; separate AAC audio tracks.
- Eighteen-second encoded recordings; fresh producer/harness processes for each
  repeat. No physical camera, desktop capture, microphone or network transport.

The normal OBS configuration was not used by the harness. No prototype plugin or
startup service was installed. Local configuration backups, recordings and host
identifiers are intentionally excluded from this public repository.

## Passed checks

- Portable core: 2,021 explicit checks, active in Release builds.
- Linux core and IPC CTest suites; Debug AddressSanitizer/UndefinedBehaviorSanitizer
  runs completed without reported errors.
- Measurement helper: nine Python unit tests, on Windows and Linux.
- GitHub Actions: Windows and Ubuntu Release builds/tests, measurement tests,
  and Ubuntu sanitizer job passed for commit `9d13369`.
- Isolated OBS module loading, synthetic video rendering, separately encoded
  audio tracks, recording completion and clean source destruction.
- One finite IPC metadata probe received 241 video frames and 401 blocks per
  audio track, with maximum observed delivery lateness of about 2.27 ms. This is
  boundary metadata evidence, not encoded synchronization or a soak test.

## Encoded measurements

Flashes/tones occur at 1.00, 2.35, 4.10, 6.70, 10.15 and 14.80 seconds in the
generated capture timeline. The helper checks their non-uniform interval
fingerprint and uses decoded frame presentation timestamps. It never independently
rebases streams or searches repeating cycles for a better offset.

Positive offset means audio later than video. Millisecond figures are rounded;
AAC onset shape, 1 ms RMS windows, and video-frame quantization limit precision.
Sub-millisecond spread is not a sub-millisecond accuracy claim.

| Run | Code | Marker match | Desktop offset | Microphone offset |
| --- | --- | --- | --- | --- |
| Baseline | Before bounded short-gap silence fill | Six per track | Median +5.33 ms; range +5.33 to +6.00 | Same |
| Repeat A | `9d13369` | Six per track | Median -10.33 ms; range -10.67 to -10.00 | Same |
| Repeat B | `9d13369` | Six per track | Median +20.67 ms; range +4.33 to +21.00 | Median +12.50 ms; range 0.00 to +21.00 |
| Repeat C | `9d13369` | **Invalid: seven desktop onsets** | No accepted six-event pairing | Six onsets; combined match rejected |

Repeat B includes a within-run microphone timing change of roughly 21 ms; its
desktop median exceeds the proposed one-frame (16.67 ms at 60 fps) target. Repeat
C detected two onsets around the second desktop tone. That could reflect a split
audio event or detector behavior; the count failure is retained rather than
silently merged away. Its cause is not yet established.

Thus, neither the three-unchanged-run gate nor stable startup timing is proven.
The promising first short run must not be generalized to hardware or restarts.

## Next validation boundary

One code-level hypothesis is insufficient PCM available at an OBS mixer deadline.
OBS 32.2.2's `discard_audio` can retain a partial source buffer while advancing
its timestamp to the next mixer interval; the relevant diagnostic is disabled
in normal builds. At 48 kHz, its 1,024-frame output block spans about 21.33 ms.
Our current due-only 480-frame delivery makes this worth investigating. This is
**not a runtime trace or a proven explanation** for either failed repeat.
Sources: [OBS mixer implementation](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/obs-audio.c),
[audio block definition](https://github.com/obsproject/obs-studio/blob/32.2.2/libobs/media-io/audio-io.h).

Investigate PCM handoff, scheduling and OBS mixer behavior with diagnostics at
the same shared timeline, and distinguish an actual audio discontinuity from a
detection artifact. Do not compensate by choosing a new arbitrary fixed offset.
Then repeat unchanged encoded tests and exercise staggered creation, scene
hide/show, producer restarts, stalls and rapid mute/unmute.

Still unimplemented or unvalidated: WASAPI capture, shared network clock,
physical V4L2 ingest/calibration, loss recovery, actual adaptive resampling,
4K60 performance/color, long-run drift, restart recovery and microphone privacy
with real filters. See the [roadmap](roadmap.md).
