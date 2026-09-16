# Six-event physical A/V measurement

`tools/measure_physical.py` analyzes a trusted short recording of one complete
non-repeating cyan-flash/unique-tone reference. It does not play media, change
OBS, control devices, or apply calibration. Record the actual physical path you
want to measure; generated detector fixtures are not hardware validation.

## Requirements and usage

Use Python 3.10 or later, NumPy, and `ffmpeg` / `ffprobe` on the executable search
path. NumPy is needed for media detection, not the pure validation tests. Install
dependencies explicitly; this helper never downloads or installs anything.

```sh
python tools/measure_physical.py recording.mkv --audio-track 0
python tools/measure_physical.py recording.mkv --audio-track 0 \
  --max-median-ms 33.333 --max-offset-ms 50
```

`--audio-track` selects a zero-based audio track; other audio tracks are ignored.
Use `--ffmpeg` and `--ffprobe` to provide explicit executable paths if needed.
The second example's timing limits are illustrative, not a project-wide claim
of acceptable synchronization. Choose limits before evaluating a recording.

The JSON report goes to standard output. Decode or ambiguity errors go to
standard error. Exit codes are:

| Code | Meaning |
| --- | --- |
| 0 | Complete marker match; requested timing limits, if any, passed. |
| 1 | Invalid input, ambiguous/missing tone, dependency failure, or decode failure. |
| 2 | Marker count or independent audio/video spacing validation failed. |
| 3 | Complete marker match, but at least one requested timing limit failed. |

Without timing limits, `timing_gate.passed` remains `null`: exit zero is only a
successful measurement, not an acceptable-sync verdict. Missing or repeated
events never produce a passed timing gate or a fit from a selected subset.

## Reference format

Play the reference once, leaving time to capture all six events. Each event is a
200 ms cyan panel flash accompanied by its unique 200 ms tone:

| Event | Reference onset (seconds) | Tone (Hz) |
| --- | ---: | ---: |
| 1 | 5.00 | 660 |
| 2 | 11.35 | 880 |
| 3 | 19.10 | 1100 |
| 4 | 29.70 | 1320 |
| 5 | 43.15 | 1540 |
| 6 | 59.80 | 1760 |

The panel must be clearly visible and unobscured. Pause unrelated sound; speech,
music, echoes, or other cyan elements can make detection ambiguous. The video
detector counts cyan pixels after area scaling to 80×45. The audio detector uses
a 50 ms Hann-window spectrum, approximately 1 ms hops, and a half-peak threshold
for each tone. Every detected candidate must satisfy its duration bounds; short
or long candidates are not silently discarded to obtain the expected count.

Both audio and video must independently match the nonuniform interval sequence.
The default interval tolerance is 40 ms (`--interval-tolerance-ms`, at most
150 ms). Pairing uses all six ordered events: no cycle wrapping, phase modulo,
per-stream zero rebasing, or automatic offset correction is performed.

## Interpretation and limits

Offsets are **audio minus video**: negative means audio is early; positive means
audio is late. The report includes each event's offset, median, spread,
first-to-last change, and a descriptive linear drift fit. Six points do not prove
long-term clock stability. Repeat successful physical captures across restarts
and operating conditions before changing production calibration.

Decoded frame timestamps are retained, including negative starts. Audio samples
are mapped through their decoded frame timestamps and sample counts; the helper
does not asynchronously resample or fill gaps. Decoder skip/padding is not
subtracted a second time. Audio timestamp continuity errors above 2 ms are
rejected, while normal millisecond container quantization is tolerated. Video
timing is frame-quantized, and codec envelopes or spectral interference can move
the detected audio onset: approximately 1 ms hop spacing is not a guarantee of
1 ms physical accuracy.

Analysis is intentionally bounded: at most 120 seconds, one video stream up to
3840×2160 at a measured 20–120 fps, and a selected 1–8 channel audio track at
8–192 kHz. The recording timeline must start within 30 seconds of zero. Decoded
frame/sample counts and byte counts are checked; timestamps and PCM must be
finite, and timestamps strictly increasing. Every decoder call has a 60-second
deadline and byte limits. Nonzero decoder exits **and error-level stderr from
zero-exit decoders** are failures. Very slow machines may hit the deadline; do
not interpret this as missing media or silently relax validation.

This is a read-only tool for trusted test recordings, not a security sandbox for
untrusted media. Keep recordings private; decoder errors may contain local
paths, so review diagnostics before publishing them.

```sh
python -m unittest discover -s tests -p test_measure_physical.py
```

The tests cover validation, bounded decoder execution, and generated PCM/pixel
fixtures. NumPy detector tests are skipped when NumPy is unavailable; the pure
validation tests still run. These test results alone are not physical A/V sync
evidence.
