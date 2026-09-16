// SPDX-License-Identifier: GPL-2.0-or-later
// Synthetic scheduling tests, not browser/hardware or end-to-end validation.
import test from 'node:test';
import assert from 'node:assert/strict';
import { ReferenceTimeline, EVENTS, DURATION_SECONDS, AMPLITUDE, POLICY,
  outputPerformanceTime, outputContextTime, eventFrames, fillReferenceChannel,
  FullscreenDimensionsGate, sameDimensions, checkScheduledStart, checkTimestampQuery, checkTimestampPair,
} from '../tools/reference-timeline.mjs';

const near = (a, b, tolerance = 1e-7) => assert.ok(Math.abs(a - b) <= tolerance, `${a} != ${b}`);
// Older pure fixtures model an instantaneous API query. This test-only adapter
// supplies that explicit bracket; production requires all readings to exist.
class FixtureTimeline extends ReferenceTimeline {
  observe(sample) {
    return super.observe({ queryStartMs: sample.callbackMs, queryEndMs: sample.callbackMs, ...sample });
  }
}
const create = (runId = 'unit-run') => new FixtureTimeline({ runId, startContextTime: 10.5, sampleRate: 48000 });
function observation(elapsedMs) {
  const rafMs = 1000 + elapsedMs;
  return { rafMs, callbackMs: rafMs + 0.25,
    stamp: { contextTime: 10 + elapsedMs / 1000, performanceTime: rafMs },
    audioState: 'running', visible: true, fullscreen: true, dimensionsStable: true };
}
function step(timeline, elapsedMs) {
  const observationValue = observation(elapsedMs);
  const result = timeline.observe(observationValue);
  if (result.needsDraw && result.status === 'running') {
    timeline.confirmDraw(result.frameSequence, observationValue.callbackMs + 0.1, observationValue.callbackMs + 0.2);
  }
  return timeline.result();
}
function complete(fps = 60) {
  const timeline = create();
  for (let frame = 0; timeline.status === 'running' && frame < 70000; ++frame)
    step(timeline, frame * 1000 / fps);
  return timeline;
}

const dimensions = (width = 1920, height = 1080) => ({ width, height, dpr: 1,
  screenWidth: 3840, screenHeight: 2160 });
test('fullscreen transition must settle for 250 ms before dimensions are snapshotted', () => {
  const gate = new FullscreenDimensionsGate(1000);
  assert.equal(gate.observe(1000, dimensions()).status, 'waiting');
  assert.equal(gate.observe(1200, dimensions(3840, 2160)).status, 'waiting');
  assert.equal(gate.observe(1449, dimensions(3840, 2160)).status, 'waiting');
  const ready = gate.observe(1450, dimensions(3840, 2160));
  assert.equal(ready.status, 'ready');
  assert.equal(ready.dimensions.width, 3840);
  assert.equal(ready.stableSinceMs, 1200);
  assert.equal(ready.changes, 2);
  // A later transition while still waiting for audio resets the stable window.
  assert.equal(gate.observe(1460, dimensions()).status, 'waiting');
  assert.equal(gate.observe(1709, dimensions()).status, 'waiting');
  assert.equal(gate.observe(1710, dimensions()).status, 'ready');
  assert.equal(sameDimensions(ready.dimensions, dimensions()), false);
  assert.equal(sameDimensions(null, dimensions()), false);
});

test('fullscreen settling is bounded and invalid metadata fails instead of arming', () => {
  const gate = new FullscreenDimensionsGate(0);
  for (let now = 0; now <= 3000; now += 200)
    assert.equal(gate.observe(now, dimensions(now % 400 ? 3840 : 1920)).status, 'waiting');
  assert.equal(gate.observe(3001, dimensions()).reason, 'fullscreen_settle_timeout');
  assert.equal(gate.observe(4000, dimensions()).status, 'invalid');
  for (const bad of [null, { ...dimensions(), width: 0 }, { ...dimensions(), dpr: NaN },
    { ...dimensions(), dpr: 20 }, { ...dimensions(), width: 16385 }]) {
    const invalid = new FullscreenDimensionsGate(0);
    assert.equal(invalid.observe(0, bad).reason, 'invalid_display_dimensions');
  }
  const backwards = new FullscreenDimensionsGate(100);
  assert.equal(backwards.observe(99, dimensions()).reason, 'invalid_startup_clock');
});

const schedule = () => ({ plannedContextTime: 2, plannedPerformanceMs: 1000, startContextTime: 2.5,
  beforeContextTime: 2.01, beforePerformanceMs: 1010,
  afterContextTime: 2.02, afterPerformanceMs: 1020 });
test('one scheduling call retains at least 250 ms of the nominal 500 ms lead', () => {
  const valid = checkScheduledStart(schedule());
  assert.equal(valid.valid, true);
  near(valid.beforeLeadMs, 490);
  near(valid.afterLeadMs, 480);
  near(valid.submissionWindowMs, 20);
  assert.equal(checkScheduledStart({ ...schedule(), beforeContextTime: 2.25, afterContextTime: 2.25,
    beforePerformanceMs: 1250, afterPerformanceMs: 1250 }).valid, true);
  assert.equal(checkScheduledStart({ ...schedule(), beforeContextTime: 2.250001,
    afterContextTime: null, afterPerformanceMs: null }).reason,
    'insufficient_audio_lead_before_start');
  assert.equal(checkScheduledStart({ ...schedule(), afterContextTime: 2.250001 }).reason,
    'insufficient_audio_lead_after_start');
});

test('past start, frozen audio clock with a long JS stall, and malformed scheduling metadata fail', () => {
  assert.equal(checkScheduledStart({ ...schedule(), beforeContextTime: 2.6, afterContextTime: 2.61 }).reason,
    'insufficient_audio_lead_before_start');
  assert.equal(checkScheduledStart({ ...schedule(), afterContextTime: 2.6 }).reason,
    'insufficient_audio_lead_after_start');
  assert.equal(checkScheduledStart({ ...schedule(), beforeContextTime: 2, afterContextTime: 2,
    afterPerformanceMs: 1501 }).reason, 'audio_schedule_submission_too_slow');
  assert.equal(checkScheduledStart({ ...schedule(), beforePerformanceMs: 1501,
    afterPerformanceMs: null, afterContextTime: null }).reason, 'audio_schedule_submission_too_slow');
  assert.equal(checkScheduledStart({ ...schedule(), afterContextTime: null }).reason,
    'invalid_audio_schedule_metadata');
  for (const field of Object.keys(schedule())) assert.equal(checkScheduledStart({ ...schedule(), [field]: NaN }).reason,
    'invalid_audio_schedule_metadata');
  assert.equal(checkScheduledStart({ ...schedule(), beforeContextTime: 1.9 }).reason, 'audio_schedule_clock_backwards');
  assert.equal(checkScheduledStart({ ...schedule(), afterPerformanceMs: 1009 }).reason, 'audio_schedule_clock_backwards');
  assert.equal(checkScheduledStart({ ...schedule(), startContextTime: 3 }).reason, 'invalid_audio_schedule_origin');
});

test('timestamp query brackets reject missing, malformed, backward and slow readings', () => {
  const bracket = { callbackEntryMs: 1000, queryStartMs: 1000.1, queryEndMs: 1001 };
  const valid = checkTimestampQuery(bracket);
  assert.equal(valid.valid, true);
  near(valid.queryDurationMs, 0.9);
  near(valid.samplingWindowMs, 1);
  for (const field of Object.keys(bracket)) for (const bad of [undefined, null, NaN, Infinity, -1, 1e13])
    assert.equal(checkTimestampQuery({ ...bracket, [field]: bad }).reason, 'invalid_audio_query_metadata');
  assert.equal(checkTimestampQuery({ ...bracket, queryStartMs: 999.999 }).reason, 'audio_query_clock_backwards');
  assert.equal(checkTimestampQuery({ ...bracket, queryEndMs: 1000.099 }).reason, 'audio_query_clock_backwards');
  assert.equal(checkTimestampQuery({ ...bracket, queryEndMs: 1010 }).valid, true);
  assert.equal(checkTimestampQuery({ ...bracket, queryEndMs: 1010.0001 }).reason, 'audio_timestamp_query_too_slow');
  // A preemption before entering the API is included, not hidden by a fast call.
  assert.equal(checkTimestampQuery({ ...bracket, queryStartMs: 1011, queryEndMs: 1011 }).reason,
    'audio_timestamp_query_too_slow');
  const missing = new ReferenceTimeline({ runId: 'missing-query', startContextTime: 10.5, sampleRate: 48000 });
  assert.equal(missing.observe(observation(0)).reason, 'invalid_audio_query_metadata');
});

test('Chrome delayed-read fixture uses post-return pair distance under explicit policy v2', () => {
  const previous = { rafMs: 623774.2, callbackMs: 623774.3999999985,
    stamp: { contextTime: 6.878584, performanceTime: 623772.700000003 },
    audioState: 'running', visible: true, fullscreen: true, dimensionsStable: true };
  const next = { ...previous, rafMs: 623782.7, callbackMs: 623782.799999997,
    stamp: { contextTime: 6.890713, performanceTime: 623784.8999999985 } };
  const options = { runId: 'query-race', startContextTime: 6.5, sampleRate: 192000 };
  const delayed = new FixtureTimeline(options);
  delayed.observe(previous);
  // Raw readings are exact; the +3 ms return is deliberately hypothetical.
  // The original failure did not measure query duration and cannot prove it.
  const result = delayed.observe({ ...next, queryStartMs: next.callbackMs + 0.1,
    queryEndMs: next.callbackMs + 3 });
  assert.equal(result.status, 'running');
  const frame = delayed.report().frames.at(-1);
  near(frame.timestampAgeMs, 0.9);
  near(frame.samplingWindowMs, 3);
  assert.equal(frame.callbackEntryMs, next.callbackMs);
  assert.equal(frame.performanceTime, next.stamp.performanceTime);
  near(frame.audioAtQueryEnd - frame.audioAtCallback, 0.003);

  const future = new FixtureTimeline(options);
  future.observe(previous);
  // Policy v1 rejected this 2.1 ms lead. Policy v2 explicitly checks pair
  // proximity in both directions; the original saved verdict is not rewritten.
  assert.equal(future.observe({ ...next, queryStartMs: next.callbackMs,
    queryEndMs: next.callbackMs }).status, 'running');
  near(future.report().maxTimestampAheadMs, 2.1);
  const slow = new FixtureTimeline(options);
  slow.observe(previous);
  assert.equal(slow.observe({ ...next, queryEndMs: next.callbackMs + 10.001 }).reason,
    'audio_timestamp_query_too_slow');
});

test('policy v2 accepts exact bracketed 2.2 ms coordinate lead with stable mapping', () => {
  const timeline = new FixtureTimeline({ runId: 'bracketed-coordinate-lead', startContextTime: 16.5, sampleRate: 192000 });
  const previous = { rafMs: 90656.6, callbackMs: 90656.80000000447,
    stamp: { contextTime: 16.818731, performanceTime: 90655.30000000447 },
    audioState: 'running', visible: true, fullscreen: true, dimensionsStable: true };
  const sample = { ...previous, rafMs: 90664.9, callbackMs: 90665.10000000149,
    queryStartMs: 90665.10000000149, queryEndMs: 90665.10000000149,
    stamp: { contextTime: 16.830758, performanceTime: 90667.30000000447 } };
  assert.equal(timeline.observe(previous).status, 'running');
  assert.equal(timeline.observe(sample).status, 'running');
  const report = timeline.report();
  assert.equal(report.sourcePolicyVersion, 2);
  assert.equal(report.policy.version, 2);
  assert.equal(report.policy.maxTimestampDistanceMs, 50);
  assert.equal(report.physicalSyncVerified, false);
  near(report.lastObservation.pair.signedAgeMs, -2.2);
  near(report.lastObservation.pair.aheadMs, 2.2);
  near(report.maxTimestampAheadMs, 2.2);
  near(report.maxTimestampBehindMs, 1.5);
  near(report.maxTimestampDistanceMs, 2.2);
  near(report.maxMappingStepMs, 0.027);
  near(report.frames.at(-1).audioAtQueryEnd, 16.828558);
  assert.equal(report.lastObservation.performanceTime, sample.stamp.performanceTime);
  assert.equal(report.lastObservation.queryEndMs, sample.queryEndMs);
});

test('equal translation of both pair coordinates preserves mapping but cannot evade the distance bound', () => {
  for (const shiftMs of [-50, -2.2, 0, 2.2, 50]) {
    const stamp = { contextTime: 20 + shiftMs / 1000, performanceTime: 1000 + shiftMs };
    const check = checkTimestampPair(1000, stamp);
    assert.equal(check.valid, true);
    near(check.distanceMs, Math.abs(shiftMs));
    near(check.mappingMs, -19000);
    near(outputContextTime(1000, stamp), 20);
    near(outputPerformanceTime(20.01, stamp), 1010);
  }
  for (const shiftMs of [-50.001, 50.001]) {
    const stamp = { contextTime: 20 + shiftMs / 1000, performanceTime: 1000 + shiftMs };
    const check = checkTimestampPair(1000, stamp);
    assert.equal(check.reason, 'audio_timestamp_pair_too_distant');
    near(check.mappingMs, -19000);
    const timeline = new FixtureTimeline({ runId: 'far-pair', startContextTime: 20.5, sampleRate: 48000 });
    assert.equal(timeline.observe({ ...observation(0), callbackMs: 1000, stamp }).reason,
      'audio_timestamp_pair_too_distant');
    const report = timeline.report();
    near(report.lastObservation.pair.distanceMs, 50.001);
    near(report.maxTimestampDistanceMs, 50.001);
    assert.equal(report.frameCount, 0);
    assert.equal(report.lastObservation.performanceTime, stamp.performanceTime);
  }
  for (const observationMs of [NaN, Infinity, -1, 1e13])
    assert.equal(checkTimestampPair(observationMs, { contextTime: 20, performanceTime: 1000 }).reason,
      'invalid_clock_metadata');
  assert.equal(checkTimestampPair(1000, { contextTime: 0, performanceTime: 1000 }).reason,
    'invalid_clock_metadata');
});

test('post-query scheduling cannot submit a draw before the query returned', () => {
  const timeline = create();
  step(timeline, 5480);
  const sample = observation(5499.5);
  const result = timeline.observe({ ...sample, queryStartMs: sample.callbackMs, queryEndMs: 6500.25 });
  assert.equal(result.needsDraw, true);
  const edge = timeline.report().events[0].onset;
  near(edge.callbackMinusEstimatedAudioMs, -0.25);
  near(edge.queryEndMinusEstimatedAudioMs, 0.25);
  assert.equal(timeline.confirmDraw(result.frameSequence, 6500.249, 6500.3).reason, 'invalid_draw_confirmation');

  const backwards = create();
  backwards.observe({ ...observation(0), queryEndMs: 1004 });
  assert.equal(backwards.observe({ ...observation(8), callbackMs: 1003.999,
    stamp: { contextTime: 10.002, performanceTime: 1002 } }).reason, 'clock_moved_backwards');
});

test('mapping is an invertible local estimate, with no extra latency compensation', () => {
  const stamp = { contextTime: 12, performanceTime: 3400 };
  near(outputPerformanceTime(12.1, stamp), 3500);
  near(outputContextTime(3500, stamp), 12.1);
  near(outputPerformanceTime(11.9, stamp), 3300);
  for (const bad of [NaN, Infinity, -1]) {
    assert.throws(() => outputPerformanceTime(bad, stamp));
    assert.throws(() => outputContextTime(bad, stamp));
  }
  for (const badStamp of [null, {}, { contextTime: 0, performanceTime: 1 },
    { contextTime: 1, performanceTime: Infinity }, { contextTime: 1e308, performanceTime: 1 }])
    assert.throws(() => outputPerformanceTime(1, badStamp));
});

test('exact browser floating-equality fixture is accepted without changing raw timestamps', () => {
  const timeline = new FixtureTimeline({ runId: 'floating-equality', startContextTime: 0.5, sampleRate: 192000 });
  const sample = { rafMs: 49346.4, callbackMs: 49346.39999999851,
    stamp: { contextTime: 0.8085650000000001, performanceTime: 49345.70000000298 },
    audioState: 'running', visible: true, fullscreen: true, dimensionsStable: true };
  assert.equal(timeline.observe(sample).status, 'running');
  assert.equal(timeline.report().frames[0].rafMs, sample.rafMs);
  assert.equal(timeline.report().frames[0].callbackMs, sample.callbackMs);
});

test('Firefox differently rounded rAF/callback fixture preserves raw data and continues', () => {
  const samples = [
    { rafMs: 132792.94, callbackMs: 132793,
      stamp: { contextTime: 0.4931355, performanceTime: 132756.1355 } },
    { rafMs: 132801.28, callbackMs: 132802,
      stamp: { contextTime: 0.5031355000000001, performanceTime: 132765.1355 } },
    { rafMs: 132809.62, callbackMs: 132809,
      stamp: { contextTime: 0.5131355000000001, performanceTime: 132772.1355 } },
  ].map(sample => ({ ...sample, audioState: 'running', visible: true,
    fullscreen: true, dimensionsStable: true }));
  const timeline = new FixtureTimeline({ runId: 'firefox-rounding', startContextTime: 1.02, sampleRate: 192000 });
  for (const sample of samples) assert.equal(timeline.observe(sample).status, 'running');
  const frame = timeline.report().frames.at(-1);
  assert.equal(frame.rafMs, 132809.62);
  assert.equal(frame.callbackMs, 132809);
  near(frame.audioAtCallback, 0.55);
  near(frame.audioAtRaf, 0.55062);
  assert.equal(timeline.report().events[0].onset, null);

  // Only the cross-API ordering assumption is removed, not monotonicity
  // within the callback clock (even if rAF itself is still advancing).
  const backwards = new FixtureTimeline({ runId: 'callback-backwards', startContextTime: 1.02, sampleRate: 192000 });
  backwards.observe(samples[0]);
  backwards.observe(samples[1]);
  assert.equal(backwards.observe({ ...samples[2], callbackMs: 132801.999 }).reason, 'clock_moved_backwards');
});

test('flash eligibility uses callback time, not a differently rounded rAF argument', () => {
  const due = create();
  step(due, 5480);
  const sample = observation(5499.5);
  sample.callbackMs = 6500.25;
  const result = due.observe(sample);
  assert.equal(result.needsDraw, true);
  const edge = due.report().events[0].onset;
  near(edge.rafMinusEstimatedAudioMs, -0.5);
  near(edge.callbackMinusEstimatedAudioMs, 0.25);
  assert.equal(due.confirmDraw(result.frameSequence, 6500.25, 6500.25).status, 'running');

  const notYet = create();
  step(notYet, 5480);
  assert.equal(notYet.observe({ ...observation(5500), callbackMs: 6499.5 }).needsDraw, false);
  assert.equal(notYet.report().events[0].onset, null);
  assert.equal(notYet.observe({ ...observation(5508), callbackMs: 6507.5 }).needsDraw, true);
});

test('Firefox derived estimate regression is rejected even when raw clocks never reverse', () => {
  // Exact consecutive raw readings from an aborted source report. Place the
  // fixture before the first marker; no private report metadata is required.
  const previous = { rafMs: 45213.9, callbackMs: 45214,
    stamp: { contextTime: 41.1055469, performanceTime: 45179.5469 },
    audioState: 'running', visible: true, fullscreen: true, dimensionsStable: true };
  const next = { ...previous, rafMs: 45222.24, callbackMs: 45222,
    stamp: { contextTime: 41.1055469, performanceTime: 45188.5469 } };
  const timeline = new FixtureTimeline({ runId: 'derived-regression', startContextTime: 40.5, sampleRate: 192000 });
  assert.equal(timeline.observe(previous).status, 'running');
  near(outputContextTime(previous.callbackMs, previous.stamp), 41.14);
  near(outputContextTime(next.callbackMs, next.stamp), 41.139);
  assert.equal(timeline.observe(next).reason, 'derived_audio_estimate_regressed');
  assert.equal(timeline.status, 'invalid');
  assert.equal(timeline.report().frameCount, 1);
  assert.equal(timeline.report().lastObservation.performanceTime, 45188.5469);
  assert.ok(timeline.report().events.every(event => event.onset === null));
  assert.equal(timeline.observe(previous).reason, 'derived_audio_estimate_regressed');

  // A repeated output position is not itself a backwards estimate when its
  // paired callback/performance clocks advance by equal amounts.
  const stable = new FixtureTimeline({ runId: 'derived-stable', startContextTime: 40.5, sampleRate: 192000 });
  stable.observe(previous);
  assert.equal(stable.observe({ ...next, callbackMs: 45223 }).status, 'running');
  near(stable.report().frames.at(-1).audioAtCallback, 41.14);
});

test('one-nanosecond equality tolerance does not permit a true backwards clock', () => {
  for (const field of ['contextTime', 'performanceTime']) {
    const timeline = create();
    step(timeline, 0);
    const sample = observation(16);
    sample.stamp[field] = observation(0).stamp[field] - (field === 'contextTime' ? 1e-5 : 0.001);
    assert.equal(timeline.observe(sample).reason, 'clock_moved_backwards');
  }
  const gate = new FullscreenDimensionsGate(49346.4);
  assert.equal(gate.observe(49346.39999999851, dimensions()).status, 'waiting');
  assert.equal(gate.observe(49346.398, dimensions()).reason, 'invalid_startup_clock');
});

test('six exact nonuniform onsets, distinct frequencies and finite PCM', () => {
  assert.deepEqual(EVENTS.map(event => event.seconds), [5, 11.35, 19.1, 29.7, 43.15, 59.8]);
  assert.deepEqual(EVENTS.map(event => event.hz), [660, 880, 1100, 1320, 1540, 1760]);
  const sampleRate = 8000;
  const channel = new Float32Array(sampleRate * DURATION_SECONDS);
  fillReferenceChannel(channel, sampleRate);
  const events = eventFrames(sampleRate);
  for (const event of events) {
    assert.equal(event.endFrame - event.onsetFrame, sampleRate / 5);
    assert.equal(channel[event.onsetFrame - 1], 0);
    assert.equal(channel[event.onsetFrame], 0);
    assert.equal(channel[event.endFrame - 1], 0);
    assert.equal(channel[event.endFrame], 0);
    assert.ok(channel.slice(event.onsetFrame, event.endFrame).some(value => Math.abs(value) > 0.05));
  }
  for (const value of channel) assert.ok(Number.isFinite(value) && Math.abs(value) <= AMPLITUDE + 1e-8);
  assert.ok(channel.slice(events.at(-1).endFrame).every(value => value === 0));
  assert.throws(() => fillReferenceChannel(new Float32Array(10), sampleRate));
  for (const rate of [0, 48000.5, 192001, NaN]) assert.throws(() => eventFrames(rate));
});

test('healthy finite pass records all twelve confirmed edges and never loops', () => {
  const timeline = complete();
  const report = timeline.report();
  assert.equal(report.status, 'completed');
  assert.equal(report.physicalSyncVerified, false);
  assert.equal(report.events.length, 6);
  for (const event of report.events) for (const edge of [event.onset, event.end]) {
    assert.ok(edge);
    assert.ok(edge.drawStartMs !== null);
    assert.ok(edge.callbackMinusEstimatedAudioMs >= -1e-8 && edge.callbackMinusEstimatedAudioMs <= 1000 / 60 + 0.25 + 1e-7);
    near(edge.drawMinusEstimatedAudioMs, edge.drawStartMs - edge.estimatedAudioOutputMs);
  }
  const count = report.frameCount;
  step(timeline, 150000);
  assert.equal(timeline.report().frameCount, count);
  assert.equal(timeline.report().events.length, 6);
});

test('240 Hz and 1000 Hz logs stay bounded while every callback is checked', () => {
  for (const fps of [240, 1000]) {
    const report = complete(fps).report();
    assert.equal(report.status, 'completed');
    assert.ok(report.frameCount > POLICY.maxLoggedFrames);
    assert.ok(report.loggedFrameCount < POLICY.maxLoggedFrames);
    for (const event of report.events) for (const edge of [event.onset, event.end]) {
      for (const seq of [edge.sequence - 1, edge.sequence, edge.sequence + 1])
        assert.ok(report.frames.some(frame => frame.sequence === seq), `Missing edge neighbor ${seq}`);
    }
  }
});

test('large frame gap and a dropped frame after baseline invalidate', () => {
  const first = create();
  step(first, 0);
  assert.equal(step(first, 51).reason, 'animation_frame_gap');
  const settled = create();
  for (let frame = 0; frame <= 40; ++frame) step(settled, frame * 1000 / 60);
  assert.equal(step(settled, 42 * 1000 / 60).reason, 'animation_frame_gap');
});

test('missing events are never replayed after pause, late start or a large jump', () => {
  const late = create();
  assert.equal(step(late, 6000).reason, 'first_frame_after_first_event');
  assert.equal(late.report().events[0].onset, null);
  assert.equal(step(late, 10).status, 'invalid');
  const stalled = create();
  step(stalled, 0);
  step(stalled, 6000);
  assert.equal(stalled.report().events[0].onset, null);
  assert.equal(stalled.status, 'invalid');
});

test('hidden, suspended, fullscreen loss and dimension changes are terminal', () => {
  for (const [field, value, reason] of [
    ['visible', false, 'document_hidden'], ['audioState', 'suspended', 'audio_not_running'],
    ['fullscreen', false, 'fullscreen_lost'], ['dimensionsStable', false, 'display_dimensions_changed'],
  ]) {
    const timeline = create();
    const sample = observation(0);
    sample[field] = value;
    assert.equal(timeline.observe(sample).reason, reason);
    assert.equal(step(timeline, 16).status, 'invalid');
  }
});

test('invalid, distant, backward and discontinuous clock metadata fail closed', () => {
  for (const field of ['rafMs', 'callbackMs']) for (const bad of [NaN, Infinity, -1, 1e308]) {
    const timeline = create();
    assert.equal(timeline.observe({ ...observation(0), [field]: bad }).reason, 'invalid_clock_metadata');
  }
  for (const bad of [NaN, Infinity, -1, 1e308]) {
    const timeline = create();
    const sample = observation(0);
    sample.stamp.contextTime = bad;
    assert.equal(timeline.observe(sample).reason, 'invalid_clock_metadata');
  }
  for (const offset of [-50.001, 50.251]) {
    const timeline = create();
    const sample = observation(0);
    sample.stamp.performanceTime += offset;
    assert.equal(timeline.observe(sample).reason, 'audio_timestamp_pair_too_distant');
  }
  const backwards = create();
  step(backwards, 16);
  assert.equal(step(backwards, 15).reason, 'clock_moved_backwards');
  const jumped = create();
  step(jumped, 0);
  const sample = observation(16);
  sample.stamp.contextTime += 0.011;
  assert.equal(jumped.observe(sample).reason, 'audio_mapping_jump');
  const repeatedStamp = create();
  for (let elapsed = 0; elapsed <= 112; elapsed += 16) {
    const repeated = observation(elapsed);
    repeated.stamp = observation(0).stamp;
    repeatedStamp.observe(repeated);
  }
  assert.equal(repeatedStamp.status, 'invalid');
  assert.equal(repeatedStamp.reason, 'audio_timestamp_pair_too_distant');
});

test('nearby timestamp coordinates cannot hide a stalled audio position', () => {
  const timeline = create();
  for (let elapsed = 0; elapsed <= 104 && timeline.status === 'running'; elapsed += 8) {
    const sample = observation(elapsed);
    sample.stamp.contextTime = 10;
    timeline.observe(sample);
  }
  assert.equal(timeline.reason, 'audio_timestamp_stalled');
  assert.equal(timeline.status, 'invalid');
  assert.ok(timeline.report().maxTimestampDistanceMs < 1);
  assert.ok(timeline.report().maxMappingStepMs <= POLICY.maxMappingStepMs);
});

test('draw confirmation is mandatory and cannot precede its callback', () => {
  for (const badDraw of [false, true]) {
    const timeline = create();
    let result;
    for (let frame = 0; frame <= 331; ++frame) {
      result = timeline.observe(observation(frame * 1000 / 60));
      if (result.needsDraw) break;
    }
    assert.equal(result.needsDraw, true);
    if (badDraw) {
      assert.equal(timeline.confirmDraw(result.frameSequence, 0, 1).reason, 'invalid_draw_confirmation');
    } else {
      assert.equal(timeline.observe(observation(5600)).reason, 'missing_draw_confirmation');
    }
  }
});

test('old rAF timestamp cannot hide a late actual draw submission', () => {
  const timeline = create();
  let result;
  for (let frame = 0; frame <= 331; ++frame) {
    result = timeline.observe(observation(frame * 1000 / 60));
    if (result.needsDraw) break;
  }
  const edge = timeline.report().events[0].onset;
  assert.ok(edge);
  const late = edge.estimatedAudioOutputMs + POLICY.maxEdgeLatenessMs + 1;
  assert.equal(timeline.confirmDraw(result.frameSequence, late, late + 0.1).reason, 'late_draw_submission');
});

test('draw-clock equality tolerates floating noise but preserves raw draw readings', () => {
  const timeline = create();
  let result;
  for (let frame = 0; frame <= 331; ++frame) {
    result = timeline.observe(observation(frame * 1000 / 60));
    if (result.needsDraw) break;
  }
  const callbackMs = timeline.report().events[0].onset.callbackMs;
  const start = callbackMs - POLICY.clockEqualityToleranceMs / 2;
  const end = start - POLICY.clockEqualityToleranceMs / 2;
  assert.equal(timeline.confirmDraw(result.frameSequence, start, end).status, 'running');
  assert.equal(timeline.report().events[0].onset.drawStartMs, start);
  assert.equal(timeline.report().events[0].onset.drawEndMs, end);
});

test('deliberate restart is a fresh generation; stopped run cannot append or resume', () => {
  const old = create('first');
  step(old, 0);
  old.stop();
  assert.equal(step(old, 16).status, 'stopped');
  assert.equal(old.report().frameCount, 1);
  const fresh = create('second');
  assert.equal(fresh.report().frameCount, 0);
  assert.equal(step(fresh, 0).status, 'running');
  assert.notEqual(fresh.report().runId, old.report().runId);
  assert.ok(fresh.report().events.every(event => event.onset === null));
  for (const start of [0, NaN, Infinity, 1e308])
    assert.throws(() => new ReferenceTimeline({ runId: 'bad', startContextTime: start, sampleRate: 48000 }));
});
