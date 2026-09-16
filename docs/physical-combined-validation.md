<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Combined physical video and desktop-audio validation

Checkpoint: 2026-09-15. Experimental isolated tests, not a production migration.
The microphone, restart certification, long-load soak and 4K60 output qualification
remain separate gates. Private recordings, host scripts and raw logs are not
part of this repository.

## Test boundary

The native OBS harness can now read its video from the positional IPC path and
its corrected desktop audio from `--desktop-ipc-path FILE`. Both mappings use
the receiving machine's monotonic clock, preserving capture timestamps and a
two-second presentation delay. Equal configured delays alone do not establish
content synchronization.

`--fit-video 1` fits the source into the harness's 640x360 canvas. It avoids
accidentally recording only the upper-left corner of a 3840x2160 source.
The defaults are unchanged for existing synthetic tests. This low-resolution
recording is for event measurement, **not a 4K60 encoding benchmark**.

The first physical trial used:

- Actual 3840x2160 NV12, nominal 60 fps capture from an Elgato 4K X.
- Windows eight-channel, 192 kHz loopback, explicit stereo conversion,
  original capture-anchor transport, bounded correction and delayed desktop IPC.
- An isolated libOBS process with the native adapter, 40 ms audio handoff lead,
  12 seconds of warmup and an 85-second recording. No normal OBS source changed.
- A one-pass 68-second reference: flashes at 5, 11.35, 19.1, 29.7, 43.15 and
  59.8 seconds, paired with distinct 660/880/1100/1320/1540/1760 Hz tones.
  Full-frame reference visibility was checked in the actual captured recording.
- A virtual X display using Mesa llvmpipe software graphics. This is an important
  load condition, not evidence of performance on the machine's hardware GPU.

Normal OBS was stopped for exclusive capture-device ownership, privately backed
up and restored after the finite test. Existing audio routing and microphone mute
were preserved. A browser-component error during normal OBS shutdown required
normal-mode recovery; this is separate from the isolated test's audio failure.

## Retained failed first trial

The recording completed and contained all six visual markers, but the final
1760 Hz audio event was absent. The strict analyzer rejected it. There is no
passed physical timing calibration from this run, and no cycle wrapping or
selective-event offset has been applied.

- The sender lost clock health at 65.595 seconds. Its last accepted clock
  observation was 2.0068101 seconds old, beyond the unchanged two-second gate.
  It stopped sending and rejected 5,441 later capture packets as unhealthy.
- The receiver consequently faulted `stale`, revoked desktop IPC, and stopped.
  Before that it delivered 2,942,400 corrected frames. It reported no network
  jitter drops, invalid original-anchor packets or phase fault.
- OBS initially received continuous desktop audio, then correctly filled silence
  after the missing feed. Its final 1,598,880 silence-filled samples are a failure
  consequence, **not a clean-audio pass**. The real microphone was not captured.
- Video accepted 6,555 frames but published only 6,264: 291 frames were dropped
  on transient IPC publication contention. No handoff-full or stale-publication
  loss was reported. Five startup driver-error frames/missing sequence numbers
  remained visible, so the video's overall result was degraded.
- OBS encoded 5,100 frames. This does not prove 5,100 distinct capture frames or
  eliminate the publication losses above.

The next test must distinguish missing clock replies from rejected timing
observations, retain video frames across bounded publication contention, and
repeat the complete six-event measurement. Do not solve either failed boundary
by silently increasing an offset or weakening freshness checks.

The existing isolated synthetic baseline still passed after the harness changes:
all six events on both tracks, encoded timing gates and independent raw-mixer
timing checks. That regression is not evidence of physical capture alignment.

## Bounded publication retry and repeat

The video worker now retains the same owned frame across a busy IPC write, with
one attempt per worker tick and the original 200 ms capture-age limit. The next
software-rendered trial published all 6,549 accepted frames. It recovered 261
frames after 361 busy attempts, with no handoff-full or stale-publication loss.
The five startup driver errors/sequence losses still made the overall video
status degraded. Recovery of contention is not permission to hide those errors.

That receiver completed 118 seconds, producing 5,448,000 corrected frames with
no correction fault. Private clock tracing showed 340 accepted observations,
454 replies and 114 RTT-filter rejections; the longest accepted-observation gap
was 1.071 seconds, within the unchanged two-second gate. This repeat did **not**
reproduce or prove the cause of the first clock outage. A later sender health
loss occurred only after the finite receiver/provider intentionally stopped.

All six audio tones were present, but an unrelated foreground window obscured
the first two visual events. Both the old local detector and the new public
[physical measurement helper](physical-measurement.md) rejected the four-event
video. No offset was fitted to a selectively chosen subset.

## First complete GPU-rendered event measurement

The subsequent isolated harness used the machine's NVIDIA GPU through its
existing display, while normal OBS remained stopped. No normal scene/profile
was loaded by the harness. The full reference stayed visible.

- All six video and audio events matched their independent interval patterns.
- Median audio-minus-video was **-67.167 ms**; the range was -67.333 to
  -50.333 ms. Negative means audio led video. Spread was 17 ms, approximately
  one 60-fps frame; first-to-last change was -0.333 ms.
- The requested one-frame median / two-frame individual-offset limits failed.
  This is a complete baseline measurement, not synchronized output.
- All 6,559 accepted capture frames were published; nine were recovered after
  11 busy attempts. Five startup driver errors/sequence losses remained.
- Receiver output was 5,445,600 corrected frames, no correction fault, no
  network jitter drops. OBS submitted 4,467,840 samples per channel over its
  separate lifetime with no fills, trims, skips, starvation or discontinuities.
- OBS reported one render-lagged frame. This test still encoded only 640x360,
  and is not a sustained 4K60 output or performance pass.

## Fresh-start repeats do not yet establish a stable calibration

The unchanged GPU path was repeated, followed by two fresh starts with video
presentation advanced by 60 ms (1,940 ms video delay, unchanged 2,000 ms audio
delay). This adjustment affected presentation only, not capture timestamps.
Every row below contains all six independently identified events. Positive
audio-minus-video means audio is later.

| Run | Video delay | Median audio-minus-video | Individual range | Timing gate |
| --- | ---: | ---: | ---: | --- |
| First baseline | 2,000 ms | -67.167 ms | -67.333 to -50.333 ms | Fail |
| Baseline repeat | 2,000 ms | -51.833 ms | -76.667 to -43.000 ms | Fail |
| First adjusted run | 1,940 ms | +14.667 ms | +5.333 to +23.333 ms | Pass |
| Adjusted repeat | 1,940 ms | +34.333 ms | +18.000 to +35.000 ms | Fail |

The gate requires an absolute median at most 16.667 ms and every event within
33.333 ms. The isolated passing run is **not repeatable calibration evidence**.
Do not keep retuning a constant to each new recording or promote this setting
into production.

The final repeat published all 6,556 accepted video frames, recovering one busy
write; five startup driver errors remained. The receiver produced 5,473,440
corrected audio frames without correction faults or network jitter drops. OBS
submitted 4,479,360 samples per channel, with zero fills, trims, skips, starvation
or discontinuities. Those clean counters exclude several transport failures but
do not prove timestamp-to-content alignment inside the player, mixer or encoder.

Next, compare source-submission audio timestamps against the raw OBS mixer and
rendered/encoded timing. The approximately 20 ms change between adjusted starts
is a diagnostic clue, not proof of a mixer-block error. The encoded reference
control below also does not certify the live player's HDMI-versus-audio output.

The public analyzer also measured the original encoded reference successfully:
all six events, 1.500 ms median audio-minus-video, each within 2 ms. That is a
decoder/detector control, not a measurement of HDMI or WASAPI latency.

## Instrumented OBS boundary

A further fresh start kept the same 1,940 ms video setting and added bounded
source-audio and raw-rendered-video traces. The full encoded six-event result
was +37.000 ms median, only 0.667 ms spread, and failed the timing gate.
An absolute-clock RMS-envelope comparison found a 0.0 ms source-to-mixer lag
at 0.1 ms search resolution (approximately 1 ms envelope windows), with
correlations 0.9935..0.9965 across non-silent fixed 15-second windows.
This is not a sample-accurate or acoustic measurement.

The raw mixer/video envelopes already had +36.100 ms median mismatch. Comparing
their six onsets to encoded onsets changed the A/V relationship by only
0.233..1.567 ms. The coarse RMS analysis discards short spoken-label fragments
and is not a replacement for the independently frequency-identified encoded
six-tone gate. It provides evidence that the large mismatch in **this run**
preceded the mixer/encoder boundary, rather than an assumed AAC-block offset.

The next unresolved boundary is captured content/presentation versus rendered
video. Live playback itself also needs an uncertainty budget: the tested
FFplay version's audio-sync mode uses a 40 ms minimum correction threshold and
an SDL-buffer-based audio-clock estimate. That is a plausible calibration
confound, not proof that the player caused the observed variation. See the
[matching FFplay source](https://github.com/FFmpeg/FFmpeg/blob/n8.0/fftools/ffplay.c).

## Capture-to-render marker trace

The next complete run additionally enabled `--trace-markers` on the physical
video bridge. It scored accepted NV12 capture frames before OBS, preserving
their original capture/presentation timestamps. All six capture and rendered
events matched; there were no rejected short-handoff frames. Rendered onsets
followed their intended video dates by 0.054, 0.110, 0.192, 16.092, 16.191 and
15.835 ms, consistent with the next 60-fps render tick rather than a growing
queue. Source-to-mixer envelope lag was 0.0 ms overall (one window estimated
-0.1 ms at the diagnostic's coarse resolution).

Its encoded median was +12.833 ms and passed the requested gate, but this does
not erase the prior unchanged-setting failures. The traces narrow this run's
uncertainty to the reference output/capture-content relationship plus ordinary
frame quantization; they do not independently certify hardware timestamp
accuracy. The capture clock's original software-timestamp limitations in
[video validation](video-validation.md) still apply.

## Alternate reference: rejected setup runs

The [instrumented browser reference](browser-reference.md) was added to expose
source submission timing separately from captured A/V timing. Its standalone
scheduling checks completed, but the initial combined captures showed another
display instead of its flashes. The physical analyzer rejected those recordings;
a successful browser report is not evidence that the capture device saw it.
Another source attempt aborted on a missed animation callback. No calibration
was fitted from these incomplete tests.

After the page was visibly verified through the actual capture input, Firefox
aborted at the third frame: its animation timestamp was 0.62 ms ahead of a
whole-millisecond `performance.now()` reading. This exposed a non-portable
cross-API ordering assumption in the reference, not a demonstrated bridge clock
failure. The reference now schedules from actual callback-clock observations,
retains raw animation timestamps independently, and tests this exact failure.
No browser privacy setting, bridge timing policy, or physical acceptance limit
was changed. Repeated complete physical captures are still required.

Normal OBS also exhibited a separate late shutdown crash during maintenance.
Restarting the GUI restored its controls, but that is not a clean-shutdown or
restart-reliability pass. A production migration remains gated on resolving and
testing that behavior, alongside the capture and timing gates above.

## First complete instrumented-browser capture

The regular Chrome reference then completed its scheduling checks on the
verified capture display: 8,218 animation callbacks, a 9.2 ms maximum gap, and
a 0.181 ms maximum audio mapping step. Its six draw-submission offsets from
estimated audio output ranged from 0.546 to 4.254 ms. These remain browser
estimates, not direct HDMI or acoustic timestamps.

The isolated recording was extended to 115 seconds to allow manual initiation;
sender and video diagnostics retain an explicit three-minute maximum. Their
clock-health, stale-media, queue and physical acceptance limits are unchanged.
At the previous 1,940 ms video / 2,000 ms audio delay:

- All six captured events matched. Audio-minus-video offsets were 28.333,
  46.000, 46.000, 28.333, 28.333 and 29.000 ms. Median was +28.667 ms;
  the one-frame median / two-frame individual gate **failed**.
- Source-to-mixer envelope lag was 0.0 ms overall; non-silent window estimates
  ranged from -0.2 to +0.2 ms at the coarse diagnostic resolution. Encoder
  differential shifts were within -0.479..+0.523 ms.
- Rendered marker onsets followed intended video presentation by
  1.197, 1.266, 1.340, 15.686, 15.824 and 15.979 ms. No capture-marker handoff
  frames were rejected; this again fits a render-tick boundary, not a growing
  OBS queue.
- OBS submitted 5,907,840 desktop samples per channel, with zero fills, trims,
  late skips, invalid blocks, starvation fills or discontinuities. This is a
  short clean-delivery result, not a listening-quality or long-soak verdict.

A single planned presentation-only adjustment to 1,975 ms video delay is the
next isolated experiment. It must remain fixed across fresh-start validation;
this baseline alone does not authorize a production calibration.

The first attempt at that adjustment aborted after one source marker. Chrome's
returned audio timestamp was 2.1 ms ahead of the callback-entry reading, but the
reference had not measured when the timestamp query returned. That incomplete
run remains invalid: it cannot distinguish a slow query from a future estimate,
and no offset was fitted from its single event. The reference now brackets each
query and uses the post-return reading, retaining the same freshness and
physical timing gates. See [the retained source failure](browser-reference.md#retained-unbracketed-chromium-source-failure).

The next, bracketed attempt also stopped, after two markers and 1,992 accepted
callbacks. Its returned pair was 2.2 ms ahead of the measured query return;
the measured query window was zero at the browser's exposed precision (the
maximum preceding sampling window was 0.2 ms). Query delay therefore does not
explain this occurrence. However, the affine clock mapping changed by only
-0.027 ms at that observation, with normal +8.327 ms mapped-audio progression.
The reference's asymmetric 2 ms future-coordinate heuristic, not a demonstrated
mapping discontinuity, triggered rejection. This incomplete attempt remains
invalid and does not validate the proposed delay. Correcting that source-policy
assumption requires a separately identified policy and fresh full captures;
it must not retroactively relabel failed attempts or widen the physical gate.

Source policy v2 now bounds the distance between the timestamp pair and the
post-query observation to 50 ms in either direction. This reuses the existing
hard frame/draw budget as a local extrapolation bound, not an accuracy claim.
It replaces the asymmetric 100 ms past / 2 ms future rule; all other source
checks and physical acceptance limits remain unchanged. Raw ahead/behind values
remain in reports. Synthetic regression fixtures cover the observed stable
mapping and deliberately invalid pairs, but do not validate a full live run.

## First complete source-policy-v2 capture

The next complete capture used the planned 1,975 ms video / 2,000 ms audio
presentation delays without further fitting. The source completed all six
markers across 8,218 callbacks. Maximum callback gap was 9.1 ms, query window
0.4 ms, coordinate lead 2.3 ms, coordinate age 10 ms, and mapping step 0.196 ms.
Draw-onset offsets from estimated audio output ranged from 0.022 to 7.622 ms.

All six physical markers matched independently. Encoded audio-minus-video
offsets were +0.333, +1.000, +1.000, +0.333, +1.000 and +1.000 ms: median
+1.000 ms and spread 0.667 ms. The unchanged 16.667 ms absolute-median /
33.333 ms absolute-event gate passed. These are detector outputs with video
frame quantization and approximately 1 ms audio hops, **not proof of
sub-millisecond physical accuracy** or long-term/restart reliability.

The desktop source submitted 5,915,520 samples per channel with zero fill,
trim, late, invalid, starvation or discontinuity counts. The receiver reported
no rejected packets, jitter drops, untimed-after-lock blocks, timestamp gaps or
correction faults. Five capture-driver error frames occurred during startup,
with the last at 876 ms; the video diagnostic still returned degraded status.
This timing pass does not erase that startup issue or the separate normal-OBS
shutdown fault. Fresh-start repeats at unchanged settings remain required.

## Three unchanged-setting timing passes

Two additional fresh starts of the isolated sender, receiver, video bridge and
OBS harness also completed the v2 source checks and six-event physical gate.
The machines were **not rebooted**, and production OBS was not migrated. Video
and audio delays remained 1,975 / 2,000 ms, with a 40 ms OBS submission lead.

| Pass | Median audio minus video | Event range | Within-pass spread | Timing gate |
| --- | ---: | ---: | ---: | --- |
| 1 | +1.000 ms | +0.333 to +1.000 ms | 0.667 ms | Pass |
| 2 | +4.333 ms | +4.333 to +5.333 ms | 1.000 ms | Pass |
| 3 | -2.000 ms | -2.333 to +15.000 ms | 17.333 ms | Pass |

All 18 measured events were within one nominal 60-fps frame. No delays were
retuned between passes, and no event was omitted. The third pass's last two
events changed by approximately one video frame; the table retains that
variation rather than summarizing only its favorable median. The gate remained
16.667 ms absolute median and 33.333 ms absolute individual offset.

The second and third desktop submissions were 5,920,800 and 5,918,880 samples
per channel, respectively, again without fills, trims, late skips, invalid
blocks, starvation or discontinuities. Receiver diagnostics again reported no
rejections, jitter drops or timestamp gaps. Each video start still reported
five driver-error frames, confined to the first second (last errors at 962 and
825 ms). Their degraded diagnostic statuses are not promoted to clean passes.

Source reports were independently audited: all six markers and twelve draw
edges were internally consistent; raw clock mappings, query statistics and
bounded-log omissions reproduced their reported values. Source estimator
mapping changes of approximately 1.1–1.2 ms over each pass are observations,
not a measured physical clock error or a long-term drift guarantee.

This establishes **three short isolated timing passes for desktop audio plus
the physical capture input**. The recordings contain 6,900 encoded 640×360
frames each; this is not a full-resolution 4K encoding performance result.
Real-microphone calibration, production source integration, supervised
whole-process/reboot recovery and a loaded long-duration soak remain separate
gates. The host-specific OBS shutdown issue was subsequently repaired and
checked as described below. Recordings and host deployment details remain private.

## Separate host OBS shutdown repair

The host's official OBS 32.2.2 binary loaded both OpenSSL and GnuTLS variants of
SRT. A debugger located the shutdown fault in SRT's static packet-filter-map
finalization. A local copy of the FFmpeg plugin was retargeted to the same SRT
variant already used by the distribution's FFmpeg library. Runtime checks then
showed one SRT implementation, with no unresolved plugin symbols.

The candidate passed a full GUI close and, after correcting the test harness,
a generated-picture-and-tone recording followed by another clean debugger exit.
That recording was 3840×2160 at nominal 60 fps with 48 kHz stereo AAC, approximately
12 seconds long, and decoded without errors. After a verified backup and
single-file installation, two ordinary OBS close/restart trials reported
successful process exit status zero. Existing scene identities, audio routes,
recording directory and all ten audio-source mute states were verified restored
and unchanged at handoff; the microphone remained muted.

This is a **host-local binary-linkage workaround, not an official OBS update or
a portable upstream fix**. A later OBS update may replace it. The short generated
recording is not a sustained 4K60 performance qualification, physical A/V timing
measurement, public-stream test, microphone calibration or bridge migration.

Two harness lessons were retained: an alternate configuration environment
variable alone did not isolate this deployed frontend's settings writes; the
corrected test used a process-private configuration mount and verified directory
identity before mutations. Also, recording-start acceptance preceded the active
recording state, so the helper now waits for bounded start/stop confirmation.
Temporary test-setting changes from the initial failed isolation trial were
restored and the persisted scene/profile data compared against the pre-test copy.
