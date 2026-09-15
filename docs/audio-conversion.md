# Explicit Windows audio conversion policy

This document specifies the synthetic/offline conversion contract. It does not
claim that a live endpoint, network path, or stream has passed these checks. The
source audit below is pinned to GStreamer 1.28.6; negotiated runtime formats must
still be checked. No custom resampler or production sample-mixing loop is needed.

## Channel identity comes before mixing

Windows `WAVEFORMATEXTENSIBLE.dwChannelMask` orders interleaved channels by
ascending set bits. Validate the subtype, container/valid bits, block alignment,
rate, channel count, and mask before passing captured memory into GStreamer.
Unknown multichannel layouts must fail closed, not acquire a guessed speaker
order. [Microsoft WAVEFORMATEXTENSIBLE](https://learn.microsoft.com/en-us/windows/win32/api/mmreg/ns-mmreg-waveformatextensible)

The common eight-channel Windows mask **0x63F is GStreamer mask 0xC3F**. Never
copy this mask numerically: GStreamer reserves position 9 for a second LFE, so
its side speakers occupy different bits. For this layout the sample order itself
already matches GStreamer's canonical order. Build `GstAudioInfo` from the actual
positions and use `gst_audio_info_to_caps()`; validate any other input order before
reordering its samples. [GStreamer channel-position definitions](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-channels.h#L93-L121),
[GstAudioInfo API](https://gstreamer.freedesktop.org/documentation/audio/audio-info.html)

| Input column | Windows bit | GStreamer position / bit | Left weight | Right weight |
|---|---|---|---|---|
| FL | 0x001 | FRONT_LEFT / 0 | 1 | 0 |
| FR | 0x002 | FRONT_RIGHT / 1 | 0 | 1 |
| FC | 0x004 | FRONT_CENTER / 2 | k | k |
| LFE | 0x008 | LFE1 / 3 | 0 | 0 |
| BL | 0x010 | REAR_LEFT / 4 | k | 0 |
| BR | 0x020 | REAR_RIGHT / 5 | 0 | k |
| SL | 0x200 | SIDE_LEFT / 10 | k | 0 |
| SR | 0x400 | SIDE_RIGHT / 11 | 0 | k |

Here `k = sqrt(0.5)`. These are explicit **project choices**, not a claim of a
certified broadcast downmix. For supported layouts containing back-center
(Windows 0x100, GStreamer REAR_CENTER/8), its initial weight is 0.5 in each output.
Only these named positions are initially allowed, with at most eight channels.
Unpositioned multichannel audio and layouts containing only LFE are rejected.

## Shared normalization and LFE policy

Multiply both rows by the same gain:

`g = 0.9 / max(sum(abs(left weights)), sum(abs(right weights)))`

Shared normalization preserves the intended left/right balance. For 0x63F,
`g = 0.28833951691533666`, and `g*k = 0.20388682769488778`. Each row's absolute
sum is 0.9. Thus inputs bounded by +/-1 cannot exceed +/-0.9 **at the matrix
output**. This is about 0.92 dB of sample headroom, not a guarantee against later
filter/inter-sample overshoot. Check peaks after resampling and before integer
packing; do not hide clipping with automatic gain changes. Stereo FL/FR uses
0.9 on the diagonal; known non-directional mono is duplicated at 0.9.

LFE is deliberately excluded in this milestone. Low-frequency content already
present in the main channels remains; this is not bass management. LFE-only
effects will be absent from the stereo mix. Including LFE later needs an explicit
gain/filter decision, listening check, and revised headroom tests. No extra
connections or changes to the local surround speaker arrangement are implied.

## Use mature conversion elements

Recommended element order:

`appsrc -> audioconvert(explicit matrix) -> F32 stereo -> audioresample -> audioconvert -> S24BE/48kHz/stereo`

`audioconvert`'s `mix-matrix` is a nested `GST_TYPE_ARRAY`: **output rows, input
columns**. Build its coefficients as `G_TYPE_FLOAT`. Explicit matrices take
precedence over automatic reordering. Do not set an empty matrix: that requests
an identity mapping, not this downmix. [audioconvert documentation](https://gstreamer.freedesktop.org/documentation/audioconvert/index.html)

The lower-level `GstAudioConverter` option is named
`GST_AUDIO_CONVERTER_OPT_MIX_MATRIX`; it uses the same shape. The 1.28.6
implementation accepts float/double/integer entries, although floats match the
element's documented example and avoid depending on newer permissiveness.
[Converter matrix validation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-converter.c#L718-L790)

Configure `audioresample` explicitly with `quality=10`, `resample-method=kaiser`,
and `sinc-filter-mode=auto` before PLAYING. Keep processing float until the final
integer conversion. Its table mode trades initialization/memory against CPU;
the documented 44.1-to-48 kHz speed example is not a measured performance claim
for this 192-to-48 kHz workload. [audioresample documentation](https://gstreamer.freedesktop.org/documentation/audioresample/index.html)

For deterministic fixtures use no dithering/noise shaping. Production 24-bit
packing policy must be explicit; use GStreamer's conversion rather than hand-
packing or truncating floats. This fixture's primary output is F32LE so peaks and
alias leakage remain observable before any integer clipping.

## Timestamp and latency contract

Keep the first-sample WASAPI QPC timestamp distinct from callback arrival time.
Appsrc timestamps must use the agreed shared-clock/running-time mapping, with
automatic arrival-time timestamping disabled. Resampler output does not have a
one-input-buffer/one-output-buffer identity.

In the audited element, output PTS is `t0 + round(output_sample_count/rate)`;
discontinuity initializes `t0` from input PTS. Filter look-ahead is reported
separately in latency queries. **Do not add/subtract that filter latency again
from output PTS.** Derive packet/reference timestamps from each actual
post-conversion output PTS, including offsets introduced by packetization; never
copy the same first-input-block reference timestamp onto several outputs.
[Resampler output/discontinuity/latency implementation](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c#L783-L981)

The raw `GstAudioConverter` API processes samples, not `GstBuffer` timestamps.
`get_out_frames()`/`get_in_frames()` account for converter state; maximum latency
is expressed in input frames. A caller choosing this lower-level API would own
the timestamp ledger and drain handling. The element path avoids reimplementing
that bookkeeping. [GstAudioConverter API](https://gstreamer.freedesktop.org/documentation/audio/gstaudioconverter.html)

Fixed-ratio `audioresample` is not adaptive device-clock correction. The audited
element tolerates gradual timestamp/sample-count disagreement up to approximately
31.25 ms before recognizing a discontinuity. That threshold must not become the
project's sync target. Long-running QPC/device-clock drift still needs explicit
measurement and bounded correction or a fail/restart policy; clock transport
alone cannot fix differing physical sample rates.
[Discontinuity check](https://github.com/GStreamer/gstreamer/blob/1.28.6/subprojects/gst-plugins-base/gst/audioresample/gstaudioresample.c#L660-L691)

## Finite offline proof

`tools/audio_conversion_fixture.py create NEW_PRIVATE_DIRECTORY` creates a
1.9-second IEEE-float extensible WAV and manifest. It never invokes GStreamer,
opens a device, plays sound, or contacts the network. The WAV contains one 1 kHz
tone and one centered impulse per input channel, correlated full-scale tones,
an 18 kHz passband tone, and a 30 kHz stopband tone. Its Windows channel mask stays
0x63F; `wavparse` must negotiate the corresponding GStreamer positions.

In that new directory, run a **separate, offline** GStreamer pipeline, substituting
the exact generated matrix values. This example never uses an audio sink:

```sh
gst-launch-1.0 -v -e filesrc location=input.wav ! wavparse ! audioconvert dithering=none noise-shaping=none \
  mix-matrix="<<(float)0.2883395169,(float)0,(float)0.2038868277,(float)0,(float)0.2038868277,(float)0,(float)0.2038868277,(float)0>,<(float)0,(float)0.2883395169,(float)0.2038868277,(float)0,(float)0,(float)0.2038868277,(float)0,(float)0.2038868277>>" \
  ! "audio/x-raw,format=F32LE,layout=interleaved,channels=2,channel-mask=(bitmask)0x3" \
  ! audioresample quality=10 resample-method=kaiser sinc-filter-mode=auto \
  ! "audio/x-raw,format=F32LE,rate=48000,channels=2" ! filesink location=converted.raw
python tools/audio_conversion_fixture.py analyze manifest.json converted.raw
```

The last command's script path is relative to the repository; adjust it when
running from the fixture directory. Keep converted output inside the new fixture
directory. Record the GStreamer version and negotiated caps with the result.
On Windows, use forward slashes in paths passed to GStreamer's pipeline parser.
For an isolated SDK, set its `bin` first in the process-local `PATH`, point
`GST_PLUGIN_PATH_1_0`, `GST_PLUGIN_SYSTEM_PATH_1_0`, and
`GST_PLUGIN_SCANNER_1_0` at that SDK, and use a private `GST_REGISTRY_1_0` file.
Do not change the persistent user/system environment for an offline check.

Acceptance is finite: exact output frame count after EOS drain; correct output
side/gain for every channel (0.2% relative tolerance, 1e-6 absolute floor); LFE
silence; impulse peak within one 48 kHz sample of its original time, measured in
the central 40 ms to exclude neighboring tone-boundary filter tails; 18 kHz gain
within 2%; 30 kHz rejection at least 70 dB relative to the nominal same-channel
1 kHz reference; all samples finite and peak below full scale. These are project
test gates, not advertised library guarantees. Repeat through S24BE packing and
back to float before accepting the actual wire conversion path.
For that repeat, insert this chain immediately before the file sink and choose
a new output filename:

```text
! audioconvert dithering=none noise-shaping=none
! audio/x-raw,format=S24BE,rate=48000,channels=2
! audioconvert dithering=none noise-shaping=none
! audio/x-raw,format=F32LE,rate=48000,channels=2
```

### Measured offline results

On 2026-09-15, the actual installed Linux runtime and isolated Windows SDK were
run against the generated 192 kHz, eight-channel F32 fixture. Both negotiated
the required input mask `0xC3F`, then 192 kHz F32 stereo, then 48 kHz F32 stereo;
the packed repeat additionally negotiated 48 kHz stereo S24BE. All four runs
drained to EOS with exactly 91,200 output frames and passed all 19 segments.

| Runtime | F32 peak | S24BE roundtrip peak | F32 30 kHz residual RMS |
|---|---:|---:|---:|
| Linux GStreamer 1.24.2 | 0.90028590 | 0.90028596 | 9.34e-9 |
| Windows GStreamer 1.28.6 | 0.90028584 | 0.90028584 | 9.42e-9 |

Both passed channel/side gain, LFE exclusion, impulse timing, 18 kHz passband,
finite-sample and clipping gates. The measured stopband residual is about
134.7 dB below the nominal same-channel reference **for this fixture**; the
24-bit roundtrip residual quantized to zero, not proof of infinite rejection.
The slight peak above 0.9 is expected filter overshoot within reserved headroom.
Impulse checks use the guarded center because neighboring tone boundaries
produce legitimate filter tails; regression tests still reject center cross-talk
and shifted impulses.

These are file-conversion measurements, not WASAPI capture, variable-buffer PTS,
RTP timestamp transport, long-run drift, microphone, or physical A/V proof.

The checked-in unit tests validate policy rejection, Windows/GStreamer mask
translation, normalization, WAV headers, and whether the analyzer rejects
deliberately swapped/shifted/clipped/aliased synthetic outputs. Passing them does
**not** test the installed GStreamer engine or the Windows sender. A later
appsrc/appsink integration check must additionally vary input buffer sizes,
verify output PTS/sample-count continuity and EOS drain, and test explicit
discontinuity/restart with a non-grid-aligned initial timestamp.
