# Generated network audio recording validation

This is an **audio-only test seam**, not physical capture or A/V calibration.
Unlike the existing direct-IPC synthetic producer, it exercises real RTP/L24,
RTCP, original capture-anchor metadata, shared-clock mapping, receiver correction,
desktop IPC, the isolated OBS adapter, and encoded desktop audio.

## Explicit generated sender

Build with `AVSYNC_BUILD_NETWORK=ON` and the separate opt-in
`AVSYNC_BUILD_NETWORK_FIXTURE=ON`. This adds `avsync-network-fixture-sender`;
ordinary builds do not acquire a new test sender implicitly. Testing also requires
a Python 3 interpreter for the prebuffered control-pipe checks.

```text
avsync-network-fixture-sender --generated-audio --host IPV4
  --clock-port N --rtp-port N --rtcp-port N
  --clock-epoch U64 --sender-session U64 --seconds 1..180
  --fixture-id 1|2 [--control-stdin]
```

The endpoint and identities must come from the explicitly started matching
receiver. Do not invent a clock epoch or adopt an unrelated clock provider. Ports
must be distinct, unprivileged and within range; multicast and unspecified
destinations are refused. No capture device, microphone, playback endpoint,
wall-clock adjustment or OBS profile is opened. The fixture uses no misleading
`--loopback` flag: generated frames are not WASAPI captures.

The overall duration includes acquisition. A short run may generate no markers.
After clock qualification the fixture emits 48 kHz stereo S24BE in finite
480-frame quanta. Ten seconds of generated silence lets the receiver acquire
its original-anchor rate estimate. Six 30 ms tones then start at nonuniform
offsets 1.000, 2.350, 4.100, 6.700, 10.150 and 14.800 seconds after that warmup.
Roles A and B use distinct frequencies, with one-ms onset/offset envelopes.
Silence continues afterward so a caller can allow the fixed playout delay to
drain before stopping.

The source's scheduled local sample timestamp is mapped **once** to the provider
clock using the same capture mapper as the real sender. Each packet carries its
original anchor separately from nominal RTP sample progression. The first tone
sample's exact mapped date is retained in its marker record; subsequent clock
calibration is not retroactively applied. Clock loss, stale pacing, source queue
overflow, ledger overflow or bad anchor association fails visibly rather than
silently resetting or concealing a gap.

`AVSYNC_CONTROL` retains the existing finite stdin lease/STOP protocol. It is
process agreement only. `AVSYNC_FIXTURE` is a separate bounded manifest prefix:
one header binds source role, session/provider/generation and fixed waveform
layout; each marker includes its event number, source frame position, original
capture date, duration and frequency. Marker output occurs only after all its
generated frames were pushed to appsrc. It does **not** prove transmission,
receiver acceptance, correction, or IPC publication. Final native summaries
label counters `generated_*`, not `captured_*`, and keep `media_verified: false`.

## Isolated recording runner

The Linux-only runner is explicitly opt-in and needs the network/ASRC receiver,
generated sender, isolated OBS smoke recorder/adapter, Xvfb, FFmpeg/ffprobe and
a working user systemd manager. Supply your own build and installed OBS paths:

```text
python3 tools/run_network_recording.py --run-loopback --case restart
  --network-build-dir NETWORK_BUILD --obs-build-dir OBS_BUILD
  --obs-plugins OBS_PLUGIN_DIRECTORY --obs-data OBS_DATA_DIRECTORY
  --output-dir NEW_PRIVATE_DIRECTORY
  --clock-port FREE_PORT_1 --rtp-port FREE_PORT_2 --rtcp-port FREE_PORT_3
```

Use `--case baseline` for the six-tone, single-generation check. The three ports
must be distinct and unprivileged; traffic is restricted to IPv4 loopback. The
output directory must not already exist. Artifacts are private by default and
include the encoded recording, raw mixer trace, bounded child logs and a result.
Do not commit them. A baseline records 45 seconds; a restart records 75 seconds.
Acquisition/control has a 115-second overall bound; decoding has a separate
100-second bound, and cleanup has finite independent retirement budgets.

Every child starts in its own session with capped, independently drained logs.
The helper/state directory that starts each transient receiver is also used to
verify its exact retirement. A successor never starts without that proof. The
generated sender's final counters and healthy receiver's native summary must
also qualify; the interrupted predecessor's unavailable summary stays unknown.
Primary failures survive cleanup or report-persistence failures. No failure
triggers retries with new timing offsets or changes the normal OBS configuration.

The runner starts a private X server and isolated libOBS recorder with black
video; it does not open a physical capture or microphone. The restart kills only
its owned receiver launcher after A3 has played, retires that receiver and then
starts a fresh pair. This is a local-network test, not cross-host recovery or a
production supervisor. User playback is unnecessary.

## Recording analysis

```text
python3 tools/measure_network_recording.py --recording PRIVATE.mkv
  --predecessor-log PRIVATE.log [--successor-log PRIVATE.log]
  --mixer-trace PRIVATE.mkv.mix0.csv
```

A baseline requires exactly A1–A6. A restart requires exactly A1–A3 then B1–B6,
with fresh sender and provider identities. The entire encoded desktop track is
inspected. Unknown, mixed, missing, duplicated, reordered or extra strong regions
fail; no convenient subset is selected. The encoded complete timeline must match
the original manifest within 40 ms using only one common unknown file origin.
The successor is never independently rebased onto its own first tone.

That relative encoded test alone cannot establish latency. A separate raw OBS
mixer trace must be continuous across the full marker span, include silence
before/after it, and place every onset within **2 ms** of its original mapped
capture date plus the fixed **2,000 ms** receiver presentation delay. Dropped
trace intervals, duplicate timestamps, incomplete coverage and tone-only islands
are rejected. Input bytes, line lengths, columns, rows, duration and decoded PCM
are bounded before analysis. Error reports omit private paths and identities.

No video stream is analyzed. The isolated recorder can encode a black picture;
its unused secondary generated audio source is not a real microphone. The
one-ms RMS detector and AAC envelopes limit precision. These checks establish
selected generated marker identity and timing, **not** channel-by-channel
fidelity, arbitrary low-level pops, representative load stability or perceptual
quality throughout the recording.

## Stale-queue claim boundary

The initial restart test allows A3 to be presented, then interrupts before A4 is
captured. Seeing A1–A3 and B1–B6 proves the tested delivery/replacement sequence;
it does not prove retirement of an already-queued A4. The analyzer therefore
reports `queued_stale_replay_verified: false` even on success.

A later queued-stale test needs an independent receiver IPC-publication witness
through A4's final sample, a known original presentation deadline and sufficient
replacement margin. A sender-side marker log is not that witness. The existing
direct-IPC [restart recording](restart-recording-validation.md) has its own
queue barrier and must not be substituted as network evidence.

## Evidence recorded so far

Windows Release and Linux builds succeeded. Offline checks reject thirteen bad
argument configurations and confirm that prefilled STOP/EOF produce zero generated
frames before network initialization. Thirty-one generated analyzer tests cover
schema/type/identity bounds, missing/replayed tones, timing shifts, continuous raw
coverage, bounded trace parsing and diagnostic privacy. None opens live media.

One isolated Linux-loopback network baseline recorded 45.034 seconds. All six
distinct tones appeared. Encoded complete-timeline error was at most 0.666 ms;
raw mixer onset error was -0.359..+0.313 ms against original capture plus delay.
The receiver reported 26 usable anchor measurements and 1,148,160 corrected stereo
frames published. One bounded IPC busy retry was reported; the final IPC failure
code was `none`, with no reported late-packet or invalid-anchor errors. Independent
retirement verified the exact receiver after STOP. This is **one local-network
generated baseline**, not Windows capture, physical A/V or a reliability soak.

A separate 75.029-second recording kept one isolated OBS instance running while
the first receiver launcher was killed and its exact native invocation was
independently retired before replacement. It contained exactly A1–A3 then B1–B6.
Encoded complete-timeline error was at most 1.068 ms; raw onset error was
-0.646..+0.432 ms against the original mapped presentation dates, using the same
2,000 ms delay and 40 ms OBS handoff lead as the baseline. Both retirement proofs
passed. The successor receiver reported 26 usable measurements, 1,148,160
corrected stereo frames and no IPC, invalid-anchor or late-packet error. The lost
first receiver's final summary remains unavailable; successful encoded markers
are not used to invent that missing diagnostic.

The public reusable runner then passed both modes independently: its 45.034-second
baseline contained A1–A6 (raw error -0.437..+0.237 ms, encoded full-timeline error
below 1.000 ms); its 75.029-second restart contained A1–A3 then B1–B6 (raw error
-0.513..+0.287 ms, encoded error below 0.808 ms). Every required retirement proof
passed, both generated senders' final counters qualified, and each available
healthy receiver summary qualified with no reported IPC, invalid-anchor or
late-packet error. No timing setting or measurement gate changed between runs.
Twenty-six mocked runner tests additionally cover isolation, bounded output,
proof refusal, generation-specific summaries, cleanup and diagnostic retention.

An earlier attempt stopped before audio startup because the private test launcher
selected the wrong installed OBS plugin directory. It also revealed a missing
new-session flag in that launcher's cleanup integration. Both were corrected;
the exact leftover test display was identity-checked and terminated, and the
failed attempt was retained. No media timing offsets were changed to obtain the
subsequent pass. Normal OBS, capture devices and startup remain untouched.
