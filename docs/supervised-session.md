<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Supervised desktop/video session

This is an **experimental, manually started session**, not a production release,
unattended service, startup installer, or recovery certification. The current
operator-directed priority is reversible normal-OBS integration and physical
A/V measurement **before** the remaining recovery qualification. Changing the
order of work does not complete or waive those qualification gates.

The scope is the physical capture picture and Windows desktop audio. Microphone
integration/calibration remains deferred; the presence of a microphone source
type in the adapter does not establish a tested real-microphone path.

## Explicit finite lifetime

The existing short diagnostic mode remains limited to `--seconds 1..180`.
Longer sessions require a distinct, deliberate opt-in:

- The private `tools/process_pair.py` configuration sets `session_mode: true`.
- `total_seconds` is at most **43,200 seconds (12 hours)**, including the
  coordinator's reserved cleanup budget. It must still exceed the configured
  cleanup/retirement reserves; it is not a promise of 12 hours of delivered media.
- `max_attempts` is **1**, also the default in session mode. There is no automatic
  pair replacement or hidden retry after a fault.
- Both native argv templates use `--session-seconds {seconds}` instead of
  `--seconds {seconds}`. The two duration flags are mutually exclusive.
- The sender still requires explicit `--loopback`, a fresh `--sender-session`,
  the receiver's fresh `--clock-epoch`, and `--control-stdin`. The receiver
  requires the pinned sender session and stdin control. The existing five-second
  lease, EOF/invalid-command behavior and bounded handshake/STOP deadlines remain.
- A remote receiver requires an independent `retire_argv`. Its launch and
  retirement commands must refer to the **same** trusted authenticated host,
  account, helper and private state directory. A matching token from a different
  empty state directory does not prove retirement of the real attempt.

For `tools/remote_receiver.py start`, use `--session-seconds N` and a private
receiver argv template containing the matching `--session-seconds {seconds}`.
The legacy short mode is unchanged. The helper retains the validated duration
mode in its private launch intent; callers must not alter existing intent files.
See [remote containment](remote-receiver-control.md) for the proof and trust
boundary, and [process-pair control](process-pair-control.md) for the base private
configuration, exact placeholder rules, OS lock and result interpretation.

The physical video producer has its own explicit
`--capture --session-seconds N --control-stdin` mode, also bounded to 12 hours.
It still needs an explicit device and a new private memory-backed runtime
directory. Session mode does not allow the finite `--trace-markers` fixture;
short instrumented diagnostics retain their existing separate mode. The desktop
pair coordinator does **not** manage the video producer or OBS. The site-specific
manual launcher must supervise their lifetimes as well; this is not a packaged
three-process service.

Start the coordinator deliberately with its existing CLI:

```text
python tools/process_pair.py --config PRIVATE_CONFIG.json --lock-file ABSOLUTE_PRIVATE_LOCK_PATH
```

This is not a ready-to-deploy configuration. Actual addresses, capture-device
choices, credentials, filesystem locations and OBS collections stay private.
Do not put credentials in command arguments or publish runtime reports unreviewed.

## Failure behavior and operator responsibility

A known desktop session/control/timing failure ends the attempt; an operator must
inspect the retained failure before deliberately starting a fresh session. Session mode
does not opt into same-sender generation recovery or clock-pause fixtures.
Readiness and native summary counters still do **not** prove audible output,
gapless delivery or acceptable physical A/V timing. Closing a controller is not
a successful stop verdict: the lease and containment policies bound cleanup,
and remote retirement must still be checked.

The finite deadline is not a hard real-time guarantee against a hung kernel or
uninterruptible device. There is no durable whole-controller reconciliation or
automatic reboot recovery claim. Keep an operator available and retain a usable
rollback path. No scheduled task, login startup or automatic restart policy is
authorized or certified by this mode.

## Reversible normal-OBS integration

Before a supervised normal-profile trial:

1. Confirm no stream/recording is active, retain a private backup, identify the
   exact profile/collection and establish rollback before changing sources.
2. Rebuild compatible IPC producers/readers and the adapter together. Release the
   physical capture device from its previous owner before the video bridge opens
   it. Verify the device's current supported format rather than assuming its
   settings survived a cable reset or reboot.
3. Preserve scene/source identities, transforms, keyboard/Stream Deck controls,
   audio routing and existing mute state where applicable. Avoid capturing the
   old and new desktop feeds simultaneously. Leave the real microphone alone.
4. Check actual picture and desktop sound in the intended normal OBS path, then
   record one complete physical reference with unchanged timing settings.
5. Retain failures, measurement output and rollback state. A successful short
   session is limited evidence about that tested path, not an unattended release.

The adapter's displayed source types are now `AV Sync Bridge - Video
(Experimental)`, `AV Sync Bridge - Desktop Audio (Experimental)`, and `AV Sync
Bridge - Microphone (Experimental)`. The persistent OBS source identifiers remain
`avsync_prototype_video`, `avsync_prototype_desktop` and
`avsync_prototype_microphone`. Display-label changes do not migrate settings,
change the IPC format, or authorize microphone capture.

## Physical timing gate

Reuse the [policy-v2 browser reference](browser-reference.md) and
[six-event physical analyzer](physical-measurement.md), not the generated
network-audio-only analyzer. Verify the browser is on the **actual captured
display** before starting; pause unrelated audio, leave the intended output
unchanged, and keep the single pass fullscreen and unobscured.

The previous three isolated timing passes used 1,975 ms video presentation delay,
2,000 ms desktop-audio presentation delay and 40 ms OBS handoff lead. These are a
previously measured starting point, **not a transferable production calibration**.
Keep them fixed for comparison unless a complete new measurement justifies a
deliberate change. A source report marked completed is necessary source evidence,
not a substitute for the independently captured/encoded result.

Require all six unique markers with their nonuniform timing; never align by a
cycle, select a favorable subset, or independently rebase audio/video. Request
the gates explicitly:

```sh
python tools/measure_physical.py recording.mkv --audio-track 0 \
  --max-median-ms 16.667 --max-offset-ms 33.333
```

Offsets are audio minus video. The limits are an absolute median of one nominal
60-fps frame and every event within two frames. Missing/ambiguous markers fail.
Without explicit limits the analyzer reports a measurement, not a timing pass.
The prior isolated recordings were 640x360/60; they do not qualify sustained
4K60 output, color/HDR correctness or resource headroom.

## Explicitly outstanding

The following remain open while supervised integration proceeds:

- Recorded cross-host restart and the remaining sender/receiver failure matrix.
- Whole-controller death and durable pending-attempt reconciliation.
- Already-queued stale **network** media rejection with an independent receiver
  publication witness; sender push alone is not that witness.
- Machine reboot, unattended startup/recovery and device-return qualification.
- Representative loaded soak, long-term drift, perceptual sound quality and
  sustained 4K60/color/resource validation.
- Repeated unchanged-setting physical confirmation on the final normal-OBS path.

The first complete normal-profile 4K60 physical recording matched all six markers
but failed the per-event timing limit; its median passed. The current supervised
integration is therefore not a final calibration pass. Evidence and its limits
are retained in the [checkpoint](checkpoint-2026-09-16.md) and
[physical validation record](physical-combined-validation.md).
