<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Recording across a producer restart

This is a Linux-only **generated** picture/desktop-audio test of IPC replacement
and the experimental OBS adapter. It does not qualify physical capture, network
transport, remote process containment, a real microphone, or production startup.

The older `run_synthetic_suite.py --case restart` replaces its producer during
recorder warmup. It checks reconnection, but the encoded recording does not span
that interruption. This separate runner keeps the same isolated libOBS sources
and recording alive throughout the replacement.

## Deliberate stale-media challenge

1. The predecessor produces white flashes and six distinct low-frequency tones.
   A1, A2 and A3 play. A4 is fully captured into the two-second IPC buffer, but
   its presentation deadline is still almost two seconds away.
2. The runner waits for that explicit queue barrier, then either sends SIGTERM
   or SIGKILL to its exact owned producer and reaps that same process.
3. The successor opens the same IPC path using explicit predecessor retirement.
   The runner does not unlink or replace the singleton lock. Publication must
   finish at least 500 ms before A4's deadline; otherwise the test is invalid.
4. The successor produces cyan flashes and six distinct higher-frequency tones.
   The complete recording must contain **exactly A1–A3 followed by B1–B6**.
   A4–A6, duplicates, missing events, swapped identities and unknown strong
   regions fail. No cycle adjustment or selected passing interval is permitted.

Both roles retain the fixed nonuniform marker schedule and original shared-clock
presentation timestamps. The predecessor and successor have fresh epochs; the
analyzer checks the interval across that boundary, without separately rebasing
the two halves. Video color and audio tone identity are detected independently.

The encoded desktop track must meet an absolute median audio-minus-video offset
of at most 16.667 ms **for each role**, and at most 33.333 ms for every event.
The entire raw desktop mixer trace must contain exactly nine events and each
onset must be within 2 ms of its original intended presentation time. Negative
encoded offsets mean audio is earlier. AAC envelope detection, one-millisecond
RMS windows and video-frame quantization limit measurement precision.

This tests queued stale-media rejection, not instantaneous retraction of audio
already copied/submitted to OBS. A recovery gap is expected; no gapless claim is
made. The harness also creates a generated mono track, which this desktop-only
restart assertion deliberately does not treat as real-microphone evidence.

## Explicit isolated run

Build with `AVSYNC_BUILD_IPC`, `AVSYNC_BUILD_OBS_PLUGIN` and
`AVSYNC_BUILD_OBS_SMOKE`. Install Xvfb, FFmpeg and ffprobe separately. Use a new
private output directory and the plugin/data directories of the tested OBS build:

```sh
python3 tools/run_restart_recording.py --build-dir ./build \
  --obs-plugins /path/to/obs-plugins --obs-data /path/to/obs-plugin-data \
  --output-dir /path/to/new-private-results
```

Default cases are one graceful stop and one hard crash, 28 seconds each. Repeat
`--case graceful` or `--case crash` for at most six cases. Failure stops the suite.
Each case creates a separate owned Xvfb server and direct libOBS process. Their
handles are cleaned up independently; no desktop display, normal OBS profile,
device, service, firewall or process-name kill is used.

Raw logs, clock identities, IPC and recordings stay in the private directory.
Public JSON contains relative timings and fixed diagnostics, not local paths or
absolute clock epochs. The analyzer bounds logs, decoding duration, frame/sample
counts, subprocess output and trace rows. Generated unit fixtures cover stale
replay, wrong/missing identities, invalid metadata, clock reversal, late
replacement, unknown clicks/colors, and helper-startup/cleanup failures.

## Results

The September 16 checkpoint records the actual runs and limits. Synthetic results
cannot close the physical/network restart, representative-load or final normal
OBS integration gates.
