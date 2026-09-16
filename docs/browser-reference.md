# Instrumented browser reference source

This is an **experimental calibration-tool improvement**, not a bridge redesign,
a diagnosis of existing A/V offsets, or evidence that a browser is physically
synchronized. The page reports flash draw submission versus **estimated** sink
audio time. A completed report is not an A/V synchronization pass.

`tools/av-reference.html` and its adjacent `reference-timeline.mjs` are local,
dependency-free files. They require no microphone, camera, account, upload,
installed package, device selection, permission-policy change, or persistent
storage. Fullscreen and audio start require an explicit click. The only generated
file is a JSON report downloaded when requested. Browser-managed downloads are
not an automatic application upload or saved preference.

## Run deliberately

Browsers commonly reject module imports from `file:` URLs. Generic HTTP servers
may also inherit an incorrect `.mjs` MIME type from the operating system. Use the
included restricted server, launched **from the repository root**:

```sh
python tools/serve-reference.py --port 8765
```

Open `http://127.0.0.1:8765/av-reference.html` in a visible desktop browser. Keep
the module adjacent to the HTML. The server binds only to IPv4 loopback and serves
only the two reference assets, with explicit JavaScript MIME type and no caching,
directory listing, request logging, or upload handler. No firewall change is
needed. Stop this optional local server with Ctrl+C when finished.

1. Pause unrelated audio; leave the intended existing system output unchanged.
2. Move the browser onto the display being captured, then click **Start one pass**.
   The idle status must identify **Source timing policy v2**, populated from the
   loaded module. Reload before starting if an older page is still open.
3. Leave the page fullscreen and visible until it finishes. **Stop** or Escape
   aborts it. There is no autoplay, retry, recovery, seek, or looping.
4. Expand **View last timing report** to inspect the summary and optional bounded
   frame log directly on the page, or download the JSON and retain it beside that
   run's independent capture. The view is populated only after stopping; nothing
   is posted or saved automatically.
   A timing rejection deliberately closes fullscreen and displays **Stopped by
   timing check**, followed by its reason; that is not a browser crash.
   Reports contain browser/display metadata and timing information; inspect them
   before sharing. They do not include device IDs, recordings, or page URLs.
5. Another pass requires another explicit Start. It creates a fresh audio
   context/timeline, not a continuation of the previous clock or queued media.

The audio buffer is exactly **68 seconds**, with black/silent space before and
after the six events. There is additionally a bounded fullscreen settling/clock warm-up and a 500 ms
scheduling lead. PCM is prepared once, scheduled once, stereo, peak amplitude
0.08 per channel, with a 2 ms fade at both burst edges. Frequencies, phase and
envelope are deterministic. The fades affect threshold-based onset detection;
retain the same envelope and detector between comparisons.

| Marker | Relative onset (seconds) | Tone (Hz) | Flash/tone duration |
| --- | --- | --- | --- |
| 1 | 5.00 | 660 | 200 ms |
| 2 | 11.35 | 880 | 200 ms |
| 3 | 19.10 | 1100 | 200 ms |
| 4 | 29.70 | 1320 | 200 ms |
| 5 | 43.15 | 1540 | 200 ms |
| 6 | 59.80 | 1760 | 200 ms |

These match the nonuniform fingerprint in `tools/measure_physical.py`. Flashes
are cyan on black. Flash edges are quantized to browser animation callbacks;
200 ms is the scheduled duration, not a claim about exact scanout duration.
Tone positions are rounded to sample indices at the actual context sample rate,
which is included in the report. No assumed 48 kHz conversion is hidden.

## Clock mapping and evidence boundary

For a scheduled context time `A` and a recent `getOutputTimestamp()` pair:

```js
estimatedOutputPerformanceMs = timestamp.performanceTime
  + 1000 * (A - timestamp.contextTime);
```

This follows the [Web Audio mapping definition](https://www.w3.org/TR/webaudio-1.0/#dom-audiocontext-getoutputtimestamp).
Do **not** add `baseLatency` or `outputLatency` again. Those are recorded only as
observations. The mapping is refreshed each callback near the event, rather
than extrapolating one sample for the whole pass. Tone timing is scheduled on
the audio rendering clock; a main-thread timer never starts an individual tone.

The source is created and connected **before** choosing the 500 ms scheduling
lead. The single `start()` call is bracketed by audio-context and performance
clock readings; at least 250 ms must remain before and after submission, and
the complete submission window must be at most 250 ms. Otherwise the attempt
is invalidated and the source stopped. This rejects a stalled startup rather
than letting a past scheduled time play immediately against an obsolete planned
origin. Raw readings and the check result appear in `audioSchedule`. This is a
control-thread submission check, not an observation that hardware played a tone.

The page separately records animation timestamp, callback time, actual draw-call
start/end, and the estimated audio output time at both edges. Positive
`drawMinusEstimatedAudioMs` means the draw call occurred after estimated audio
output. This is **not** the encoded recording's audio-minus-video offset and has
the opposite ordering of operands.

Each `getOutputTimestamp()` call is bracketed by `queryStartMs` and `queryEndMs`
from `performance.now()`, while `callbackMs` / `callbackEntryMs` retain the
original callback-entry reading. Flash eligibility and timestamp freshness use
the reading **after the API returns**, mapped through that returned pair.
`audioAtQueryEnd` and `queryEndMinusEstimatedAudioMs` identify the scheduling
basis; no pre-query reading is used to decide whether the returned pair is
apparently future-dated. The raw rAF argument, `audioAtRaf`, `audioAtCallback`,
`rafMinusEstimatedAudioMs` and `callbackMinusEstimatedAudioMs` are observational;
no timestamp is clamped or adjusted to manufacture ordering. In Firefox,
rendering timestamps and `performance.now()` can use different precision
reduction, so the rAF argument can legitimately be numerically ahead of a later
callback reading. Mozilla documents this behavior in
[bug 1838890](https://bugzilla.mozilla.org/show_bug.cgi?id=1838890#c4), and its
[Performance implementation](https://github.com/mozilla/gecko-dev/blob/master/dom/performance/Performance.cpp)
uses separate reduction paths. Genuine backwards progression within either
clock still invalidates the run; this is not a larger timing tolerance.

`requestAnimationFrame` uses the rendering opportunity's timestamp, before the
subsequent rendering work. It is not a compositor-present, HDMI-scanout, or
photodiode timestamp. The browser can also suppress opportunities when hidden
or overloaded. See the [HTML rendering model](https://html.spec.whatwg.org/multipage/webappapis.html#update-the-rendering).
The first eligible callback can itself add up to roughly one frame of phase
quantization in a healthy run. The page does not subtract a guessed frame.

The audio API likewise estimates device output. Chromium calculates the position
using supplied playback delay and explicitly allows that delay to be approximate
in its [audio destination implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/platform/audio/audio_destination.cc).
Its [AudioContext implementation](https://chromium.googlesource.com/chromium/src/+/HEAD/third_party/blink/renderer/modules/webaudio/audio_context.cc)
also quantizes the separate `outputLatency` property. These sources describe
upstream code, not verification of the user's installed browser build.

Mozilla's [AudioContext implementation](https://github.com/mozilla/gecko-dev/blob/master/dom/media/webaudio/AudioContext.cpp)
also shows why an output timestamp is not an independent hardware observation:
its estimate is formed from the context/performance readings and output latency.
Passing the probe does not certify that implementation's physical accuracy.

Critically, [WASAPI loopback](https://learn.microsoft.com/en-us/windows/win32/coreaudio/loopback-recording)
can copy the audio-engine mix or use a hardware loopback pin. Its capture boundary
is not necessarily the acoustic output position estimated by the browser. The
probe does not remove that distinction, AVR processing, monitor scanout, USB
capture, bridge delay, OBS processing, or encoding. Compare repeated captured
passes; do not calibrate the bridge solely from page-reported numbers.

The report contains no measured transform between DOM performance time and the
sender's native QPC/shared capture clock. `performance.timeOrigin` is metadata,
not that calibration. Do not directly subtract browser timestamps from native
capture timestamps. Relative interval and variation comparisons are meaningful;
an absolute browser-to-loopback latency remains unmeasured by this page.

## Conservative source-quality checks

The exported `POLICY` is included verbatim in every report. It is a source-test
quality policy, **not** a replacement for any bridge health or timing gate:

Current reports carry `sourcePolicyVersion: 2` and
`policy.name: "bounded-pair-mapping"`, including failures before arming. The JSON
schema label remains v1; source-policy version is a separate field. This is an
explicit behavioral change from the original unversioned policy (policy v1):
the old **100 ms behind / 2 ms ahead** restriction is replaced by a symmetric
**50 ms maximum distance** between the pair's performance coordinate and the
post-query observation. The past-side limit is tighter; the future side no
longer assumes a numerical guarantee that Web Audio never specified. Previously
saved reports and their failed verdicts are unchanged, not retroactively passed.

The 50 ms bound is an **operational interpolation/extrapolation budget**, sharing
the existing hard frame/draw budget and limiting mapping distance to a quarter
of a 200 ms marker. It is neither a Web Audio requirement nor a 50 ms accuracy
guarantee. A pair's coordinate lead is not itself its mapping error: translating
both coordinates by equal time preserves their mapping. The
[current specification](https://webaudio.github.io/web-audio-api/#dom-audiocontext-getoutputtimestamp)
defines an estimated pair and mapping formula but provides no 2 ms future or
absolute accuracy promise. Closer estimates can be better, but closeness alone
cannot certify a browser, WASAPI boundary, or physical output.

- Audio must remain running; page visible, fullscreen, and the same dimensions.
- Before arming, allow fullscreen to finish resizing: dimensions must remain
  stable for 250 ms across animation callbacks, within a three-second deadline.
  Only then is the final size snapshotted and the source scheduled. Every
  dimensions change **after** arming still invalidates the run immediately.
- Require finite, positive output timestamp pairs after at least five advancing
  warm-up observations. Warm-up/settling times out after three seconds.
- The absolute pair distance from post-query `queryEndMs` must be at most 50 ms,
  using the same `checkTimestampPair` helper during warm-up and the armed run.
  Neither member is clamped, rebased, or corrected to satisfy the limit. Each query's
  readings must be finite and ordered within the performance clock. The entire
  callback-entry-to-query-return sampling window is limited to 10 ms, including
  any preemption before the API call. This conservative measurement bound uses
  one fifth of the existing 50 ms hard frame/draw budgets. Per-frame query duration, total sampling window, aggregate
  maximum and the last bracket are retained even when the attempt is invalid.
  Nonadvancing timestamps, backwards clocks, or a mapping step over 10 ms fail.
- Same-clock ordering/equality comparisons allow only `1e-6` ms (one nanosecond)
  of numerical noise: browsers can expose equivalent readings with different
  floating-point rounding. Raw timestamps are retained, not clamped or rebased.
  This is not an accuracy claim; gap, mapping-step and draw-lateness limits
  are unchanged by policy v2. A genuinely backwards clock still invalidates the run.
  Cross-API rAF-versus-callback ordering is not required; their raw readings may
  have different privacy rounding. Scheduling and draw checks use the callback
  performance clock after the query returns, while rAF cadence remains checked
  independently. A draw must not precede that post-query reading.
  `derived_audio_estimate_regressed` separately rejects a backwards mapped audio
  estimate when all raw input clocks remain nondecreasing; it does not claim
  that a hardware clock or the bridge moved backwards.
- Reject callback gaps over 50 ms. After 30 intervals, also reject a gap exceeding
  1.5 times the initial median interval plus 2 ms. Up to 120 initial intervals
  establish that median. These are conservative heuristics, not hardware bounds.
- A missed event is never replayed. All six onsets and offsets need actual
  draw-call confirmation within 50 ms of the estimated audio edge, including
  draw completion (not just the rAF timestamp). Suspended/hidden/interrupted runs remain invalid after
  the condition clears; restarting requires another click.
- The buffer is finite even if JavaScript stalls. Stop/invalidity cancels the
  animation callback, stops/disconnects the source, and closes the context. An
  independent 72-second deadline bounds an armed run when the event loop runs.

All callbacks are checked, including high-refresh-rate displays. Detailed frame
logging is sampled at an interval of at least 8 ms, plus the callback immediately
before/on/after each edge, capped at 12,000 records. Aggregate maxima cover every
callback. Latency snapshots are bounded at 80. There is no console/file output,
download, or DOM table construction in the per-frame path. JSON serialization happens only after
stopping. The report retains an invalidity reason and last observed clock data.
Signed `timestampAgeMs` is positive behind and negative ahead. Per-frame
`timestampAheadMs`, `timestampBehindMs`, `timestampDistanceMs` and their aggregate
maxima retain the magnitudes. `maxTimestampAgeMs` remains a compatibility alias
of `maxTimestampBehindMs`. A finite pair outside the new distance bound is
included in the final diagnostics/maxima before rejection, not in accepted
frames. `lastTimestampPair` also retains the last warm-up/armed check; armed
`lastObservation.pair` includes raw members, signed distance and mapping.

`performance.now()` is monotonic, not wall-clock/NTP time, and its exposed precision
may be reduced for privacy. See [High Resolution Time](https://www.w3.org/TR/hr-time-3/).
Audio/fullscreen readiness is checked instead of assuming autoplay succeeded;
see [Chrome's Web Audio autoplay guidance](https://developer.chrome.com/blog/autoplay#web-audio).

## Tests and current limits

No package installation is required when Node.js is already available:

```sh
node --test tests/reference_timeline.test.mjs
```

These are **synthetic pure-logic tests**, covering mapping, PCM/fingerprint,
finite completion, high-rate log bounds, missing draw acknowledgments, bad clocks,
frame gaps, differently rounded Firefox clocks, post-query edge eligibility,
delayed reads, policy-v2 near/far pairs, equal-coordinate translation, unchanged
mapping/backwards/stall rejection, malformed/slow query brackets,
bounded fullscreen settling, scheduling-lead failures, hidden/paused
states and fresh restarts. They neither execute a real
browser nor prove audio output, fullscreen behavior, physical capture timing, or
end-to-end OBS behavior. Actual browser smoke tests and repeated physical captures
remain separate validation steps. CI runs the pure scheduling tests without a
browser or playback. The reference does not change bridge timing policy.

### Retained Firefox source failure

A Firefox 155 run at 192 kHz / approximately 120 animation callbacks per second
aborted after four markers. The raw output context position repeated while its
paired performance timestamp advanced by 9 ms and the separately sampled
callback clock advanced by 8 ms. Their mapped audio estimate therefore moved
backwards by 1 ms. All preceding rAF gaps were at most 8.36 ms. This is an
observed **reference-source estimate failure**, not evidence of a bridge timing
failure or proof that physical audio ran backwards. The run remains invalid;
neither its incomplete marker sequence nor a subset can calibrate the bridge.

The exact consecutive readings have a negative regression test. Rejection is
preserved with the more specific reason above; no estimator smoothing or forced
monotonic clamping was added. Policy v2 does not permit this derived reversal. Trying a regular
Chromium browser with unchanged privacy/audio settings is a separate controlled
source experiment, not an automatic fallback or a guarantee of hardware timing.
It must independently complete the source checks and physical capture gates.

### Retained unbracketed Chromium source failure

A later Chromium run rejected a returned timestamp that was 2.1 ms ahead of the
callback-entry reading. That version sampled `performance.now()` only **before**
the timestamp API, so the report cannot distinguish a delayed API query or
preemption from a genuinely future output estimate. The source run remains
invalid and provides no complete physical-calibration result.

Query bracketing fixes that measurement ambiguity; it does not establish the
cause retrospectively. The regression retains the exact old raw readings with
an explicitly hypothetical 3 ms return delay to test post-query measurement.
Under the explicitly changed policy v2, the 2.1 ms lead alone is within the new
pair-distance bound; that does not change the saved failed source run or make
it a physical calibration.

### Bracketed Chromium evidence motivating policy v2

A subsequent Chrome 153 / 192 kHz run still failed the old 2 ms rule: its pair
was 2.2 ms ahead **after** the query returned. The final bracket measured zero
elapsed time at exposed precision; the maximum query window was 0.2 ms. This
rules out a multi-millisecond API-query delay for that occurrence. Yet the pair's
mapping changed by only -0.027 ms, and the mapped audio position advanced by
8.327 ms. A 2.2 ms coordinate lead was therefore not a 2.2 ms mapping jump.

Chromium's [destination implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/platform/audio/audio_destination.cc)
can advance both pair coordinates together repeatedly during multi-quantum
rendering. That can move the stored pair ahead without changing its mapping;
larger fixed-duration buffers at high rates can contain more rendering quanta.
Its [context implementation](https://github.com/chromium/chromium/blob/main/third_party/blink/renderer/modules/webaudio/audio_context.cc)
returns the stored pair without an upper clamp of performance time to "now".
Context-position clamps and timestamp precision reduction are separate caveats.
This is a plausible upstream mechanism, **not proof of the installed browser's
exact execution path or the cause**: its buffer size, loop timings and active
clamps were not measured.

The exact observed pair is now a v2 positive scheduling regression. Pairs more
than 50 ms away, malformed/slow queries, real mapping jumps, raw or derived
backwards progress, stale progress, frame gaps and late draws still fail. The
saved aborted report remains invalid. No physical acceptance limit or bridge
health policy changed; a complete fresh source run and independent six-marker
physical measurement remain required before calibration claims.
