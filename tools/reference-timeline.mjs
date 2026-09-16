// SPDX-License-Identifier: GPL-2.0-or-later
// Pure scheduling/report logic. This does not observe physical presentation.
export const DURATION_SECONDS = 68;
export const FLASH_SECONDS = 0.2;
export const AMPLITUDE = 0.08;
export const EDGE_FADE_SECONDS = 0.002;
export const EVENTS = Object.freeze([5, 11.35, 19.1, 29.7, 43.15, 59.8].map(
  (seconds, index) => Object.freeze({ id: index + 1, seconds, hz: 660 + 220 * index })));
export const POLICY = Object.freeze({
  version: 2,
  name: 'bounded-pair-mapping',
  // Equality guard only (1 ns), not an accuracy claim or a health allowance.
  clockEqualityToleranceMs: 1e-6,
  maxLoggedFrames: 12000,
  logIntervalMs: 8,
  maxFrameGapMs: 50,
  adaptiveGapMultiplier: 1.5,
  adaptiveGapSlackMs: 2,
  // Operational interpolation/extrapolation budget, not physical accuracy:
  // same 50 ms budget as hard frame/draw checks; one quarter of a 200 ms marker.
  maxTimestampDistanceMs: 50,
  maxTimestampStallMs: 100,
  // One fifth of the existing 50 ms hard frame/draw budgets. Includes any
  // preemption between callback entry and the returned timestamp observation.
  maxTimestampQueryMs: 10,
  maxMappingStepMs: 10,
  maxEdgeLatenessMs: 50,
});
export const STARTUP_POLICY = Object.freeze({ stableDimensionsMs: 250, deadlineMs: 3000 });
export const SCHEDULING_POLICY = Object.freeze({ initialLeadSeconds: 0.5,
  minimumRemainingLeadMs: 250, maximumSubmissionWindowMs: 250 });

const finite = value => typeof value === "number" && Number.isFinite(value);
const beforeMs = (value, reference) => reference - value > POLICY.clockEqualityToleranceMs;
const beforeSeconds = (value, reference) => reference - value > POLICY.clockEqualityToleranceMs / 1000;
export function checkTimestampQuery({ callbackEntryMs, queryStartMs, queryEndMs }) {
  const result = { valid: false, reason: null, callbackEntryMs, queryStartMs, queryEndMs,
    queryDurationMs: null, samplingWindowMs: null };
  if ([callbackEntryMs, queryStartMs, queryEndMs].some(value => !finite(value) || value < 0 || value > 1e12))
    return { ...result, reason: 'invalid_audio_query_metadata' };
  if (beforeMs(queryStartMs, callbackEntryMs) || beforeMs(queryEndMs, queryStartMs))
    return { ...result, reason: 'audio_query_clock_backwards' };
  result.queryDurationMs = queryEndMs - queryStartMs;
  result.samplingWindowMs = queryEndMs - callbackEntryMs;
  if (result.samplingWindowMs > POLICY.maxTimestampQueryMs)
    return { ...result, reason: 'audio_timestamp_query_too_slow' };
  return { ...result, valid: true };
}
export function checkScheduledStart(input) {
  const { plannedContextTime, plannedPerformanceMs, startContextTime,
    beforeContextTime, beforePerformanceMs, afterContextTime = null, afterPerformanceMs = null } = input;
  const result = { valid: false, reason: null, policy: SCHEDULING_POLICY,
    beforeLeadMs: null, afterLeadMs: null, submissionWindowMs: null };
  const fail = reason => ({ ...result, reason });
  const times = [plannedContextTime, startContextTime, beforeContextTime,
    plannedPerformanceMs, beforePerformanceMs];
  if (times.some(value => !finite(value) || value < 0) ||
      [plannedContextTime, startContextTime, beforeContextTime].some(value => value > 1e9) ||
      [plannedPerformanceMs, beforePerformanceMs].some(value => value > 1e12) ||
      ((afterContextTime === null) !== (afterPerformanceMs === null)) ||
      (afterContextTime !== null && (!finite(afterContextTime) || afterContextTime < 0 ||
        afterContextTime > 1e9 || !finite(afterPerformanceMs) || afterPerformanceMs < 0 || afterPerformanceMs > 1e12)))
    return fail('invalid_audio_schedule_metadata');
  if (Math.abs(startContextTime - plannedContextTime - SCHEDULING_POLICY.initialLeadSeconds) * 1000 >
      POLICY.clockEqualityToleranceMs) return fail('invalid_audio_schedule_origin');
  if (beforeSeconds(beforeContextTime, plannedContextTime) || beforeMs(beforePerformanceMs, plannedPerformanceMs) ||
      (afterContextTime !== null && (beforeSeconds(afterContextTime, beforeContextTime) ||
        beforeMs(afterPerformanceMs, beforePerformanceMs)))) return fail('audio_schedule_clock_backwards');
  result.beforeLeadMs = 1000 * (startContextTime - beforeContextTime);
  if (beforeMs(result.beforeLeadMs, SCHEDULING_POLICY.minimumRemainingLeadMs))
    return fail('insufficient_audio_lead_before_start');
  result.submissionWindowMs = (afterPerformanceMs ?? beforePerformanceMs) - plannedPerformanceMs;
  if (beforeMs(SCHEDULING_POLICY.maximumSubmissionWindowMs, result.submissionWindowMs))
    return fail('audio_schedule_submission_too_slow');
  if (afterContextTime !== null) {
    result.afterLeadMs = 1000 * (startContextTime - afterContextTime);
    if (beforeMs(result.afterLeadMs, SCHEDULING_POLICY.minimumRemainingLeadMs))
      return fail('insufficient_audio_lead_after_start');
  }
  return { ...result, valid: true };
}
function positive(value, name) {
  if (!finite(value) || value <= 0) throw new TypeError(`Invalid ${name}`);
  return value;
}
function stampValid(stamp) {
  return stamp && finite(stamp.contextTime) && stamp.contextTime > 0 && stamp.contextTime <= 1e9 &&
    finite(stamp.performanceTime) && stamp.performanceTime > 0 && stamp.performanceTime <= 1e12;
}
export function checkTimestampPair(observationMs, stamp) {
  const result = { valid: false, reason: null, observationMs,
    contextTime: stamp?.contextTime, performanceTime: stamp?.performanceTime,
    signedAgeMs: null, aheadMs: null, behindMs: null, distanceMs: null, mappingMs: null };
  if (!finite(observationMs) || observationMs < 0 || observationMs > 1e12 || !stampValid(stamp))
    return { ...result, reason: 'invalid_clock_metadata' };
  result.signedAgeMs = observationMs - stamp.performanceTime;
  result.aheadMs = Math.max(0, -result.signedAgeMs);
  result.behindMs = Math.max(0, result.signedAgeMs);
  result.distanceMs = Math.abs(result.signedAgeMs);
  result.mappingMs = stamp.performanceTime - 1000 * stamp.contextTime;
  if (result.distanceMs > POLICY.maxTimestampDistanceMs)
    return { ...result, reason: 'audio_timestamp_pair_too_distant' };
  return { ...result, valid: true };
}
export function outputPerformanceTime(contextTime, stamp) {
  if (!finite(contextTime) || contextTime < 0 || !stampValid(stamp))
    throw new TypeError("Invalid audio clock mapping");
  const mapped = stamp.performanceTime + 1000 * (contextTime - stamp.contextTime);
  if (!finite(mapped)) throw new TypeError("Audio clock mapping overflow");
  return mapped;
}
export function outputContextTime(performanceMs, stamp) {
  if (!finite(performanceMs) || performanceMs < 0 || !stampValid(stamp))
    throw new TypeError("Invalid performance clock mapping");
  const mapped = stamp.contextTime + (performanceMs - stamp.performanceTime) / 1000;
  if (!finite(mapped)) throw new TypeError("Performance clock mapping overflow");
  return mapped;
}
export function eventFrames(sampleRate) {
  if (!Number.isInteger(sampleRate) || sampleRate < 8000 || sampleRate > 192000)
    throw new TypeError("Unsupported sample rate");
  return EVENTS.map(event => ({ ...event,
    onsetFrame: Math.round(event.seconds * sampleRate),
    endFrame: Math.round((event.seconds + FLASH_SECONDS) * sampleRate),
  }));
}
export function fillReferenceChannel(channel, sampleRate) {
  const events = eventFrames(sampleRate);
  if (!(channel instanceof Float32Array) || channel.length !== DURATION_SECONDS * sampleRate)
    throw new TypeError("Expected one finite 68-second PCM channel");
  channel.fill(0);
  const fadeFrames = Math.max(1, Math.round(EDGE_FADE_SECONDS * sampleRate));
  for (const event of events) {
    for (let frame = event.onsetFrame; frame < event.endFrame; ++frame) {
      const offset = frame - event.onsetFrame;
      const envelope = Math.min(1, offset / fadeFrames, (event.endFrame - 1 - frame) / fadeFrames);
      channel[frame] = envelope === 0 ? 0 :
        AMPLITUDE * envelope * Math.sin(2 * Math.PI * event.hz * offset / sampleRate);
    }
  }
}
const median = values => {
  const sorted = [...values].sort((a, b) => a - b);
  return sorted.length ? sorted[Math.floor(sorted.length / 2)] : null;
};

const DIMENSION_KEYS = ['width', 'height', 'dpr', 'screenWidth', 'screenHeight'];
export function sameDimensions(a, b) {
  return Boolean(a && b && DIMENSION_KEYS.every(key => a[key] === b[key]));
}
export class FullscreenDimensionsGate {
  constructor(startedMs) {
    if (!finite(startedMs) || startedMs < 0) throw new TypeError('Invalid startup time');
    this.startedMs = startedMs;
    this.lastMs = startedMs;
    this.stableSinceMs = null;
    this.dimensions = null;
    this.observations = 0;
    this.changes = 0;
    this.status = 'waiting';
    this.reason = null;
  }
  observe(nowMs, dimensions) {
    if (this.status === 'invalid') return this.result();
    if (!finite(nowMs) || beforeMs(nowMs, this.lastMs)) return this.fail('invalid_startup_clock');
    this.lastMs = nowMs;
    if (nowMs - this.startedMs > STARTUP_POLICY.deadlineMs) return this.fail('fullscreen_settle_timeout');
    if (!dimensions || DIMENSION_KEYS.some(key => !finite(dimensions[key]) || dimensions[key] <= 0) ||
        dimensions.dpr > 8 || dimensions.width * dimensions.dpr > 16384 ||
        dimensions.height * dimensions.dpr > 16384 ||
        dimensions.width * dimensions.height * dimensions.dpr ** 2 > 33554432)
      return this.fail('invalid_display_dimensions');
    if (!sameDimensions(this.dimensions, dimensions)) {
      this.dimensions = Object.fromEntries(DIMENSION_KEYS.map(key => [key, dimensions[key]]));
      this.stableSinceMs = nowMs;
      this.observations = 1;
      ++this.changes;
      this.status = 'waiting';
    } else {
      ++this.observations;
      if (this.observations >= 2 && nowMs - this.stableSinceMs >= STARTUP_POLICY.stableDimensionsMs)
        this.status = 'ready';
    }
    return this.result();
  }
  fail(reason) { this.status = 'invalid'; this.reason = reason; return this.result(); }
  result() {
    return { status: this.status, reason: this.reason, dimensions: this.dimensions,
      stableSinceMs: this.stableSinceMs, observedAtMs: this.lastMs,
      observations: this.observations, changes: this.changes, policy: STARTUP_POLICY };
  }
}

export class ReferenceTimeline {
  constructor({ runId, startContextTime, sampleRate }) {
    if (typeof runId !== "string" || !runId.length || runId.length > 100)
      throw new TypeError("Invalid run ID");
    this.startContextTime = positive(startContextTime, "audio start");
    if (startContextTime > 1e9) throw new TypeError("Audio start outside supported lifetime");
    this.sampleRate = sampleRate;
    this.runId = runId;
    this.events = eventFrames(sampleRate).map(event => ({ ...event,
      audioContextOnset: startContextTime + event.onsetFrame / sampleRate,
      audioContextEnd: startContextTime + event.endFrame / sampleRate,
      onset: null, end: null,
    }));
    this.status = "running";
    this.reason = null;
    this.frames = [];
    this.frameCount = 0;
    this.maxFrameGapMs = 0;
    this.maxTimestampAgeMs = 0;
    this.maxTimestampAheadMs = 0;
    this.maxTimestampDistanceMs = 0;
    this.maxTimestampQueryMs = 0;
    this.maxMappingStepMs = 0;
    this.initialFrameIntervals = [];
    this.previous = null;
    this.lastStampAdvanceMs = null;
    this.lastLoggedMs = -Infinity;
    this.logNext = false;
    this.pendingDraw = null;
    this.activeEvent = null;
    this.lastObservation = null;
  }
  invalidate(reason) {
    if (this.status === "running") {
      this.status = "invalid";
      this.reason = typeof reason === "string" ? reason.slice(0, 160) : "invalid_run";
      this.activeEvent = null;
    }
    return this.result();
  }
  stop() {
    if (this.status === "running") {
      this.status = "stopped";
      this.reason = "user_stopped";
      this.activeEvent = null;
    }
    return this.result();
  }
  result() {
    return { status: this.status, reason: this.reason, activeEvent: this.activeEvent,
      frameSequence: this.frameCount, needsDraw: this.pendingDraw !== null };
  }
  observe({ rafMs, callbackMs, queryStartMs, queryEndMs, stamp, audioState, visible, fullscreen, dimensionsStable }) {
    if (this.status !== "running") return this.result();
    this.lastObservation = { rafMs, callbackMs, callbackEntryMs: callbackMs, queryStartMs, queryEndMs,
      contextTime: stamp?.contextTime,
      performanceTime: stamp?.performanceTime, audioState, visible, fullscreen, dimensionsStable,
      pair: checkTimestampPair(queryEndMs, stamp) };
    if (this.pendingDraw !== null) return this.invalidate("missing_draw_confirmation");
    if (audioState !== "running") return this.invalidate("audio_not_running");
    if (visible !== true) return this.invalidate("document_hidden");
    if (fullscreen !== true) return this.invalidate("fullscreen_lost");
    if (dimensionsStable !== true) return this.invalidate("display_dimensions_changed");
    if (!finite(rafMs) || rafMs < 0 || rafMs > 1e12 || !finite(callbackMs) || callbackMs < 0 ||
        callbackMs > 1e12 || !stampValid(stamp))
      return this.invalidate("invalid_clock_metadata");
    const query = checkTimestampQuery({ callbackEntryMs: callbackMs, queryStartMs, queryEndMs });
    this.lastObservation.query = query;
    if (!query.valid) return this.invalidate(query.reason);
    this.maxTimestampQueryMs = Math.max(this.maxTimestampQueryMs, query.samplingWindowMs);
    // The returned pair may have been produced after callback entry. Freshness
    // and scheduling must reference an observation made after that API call.
    const pair = this.lastObservation.pair;
    if (pair.distanceMs !== null) {
      // Include a finite but out-of-policy final pair in diagnostics. Never
      // accept or clamp it merely to keep its statistics available.
      this.maxTimestampAgeMs = Math.max(this.maxTimestampAgeMs, pair.behindMs);
      this.maxTimestampAheadMs = Math.max(this.maxTimestampAheadMs, pair.aheadMs);
      this.maxTimestampDistanceMs = Math.max(this.maxTimestampDistanceMs, pair.distanceMs);
    }
    if (!pair.valid) return this.invalidate(pair.reason);
    // Rendering timestamps and performance.now() can be reduced differently
    // (notably in Firefox). Never impose causal order between their raw values.
    // Schedule using the post-query performance clock, as for draw timing;
    // keep rAF and callback entry as unmodified observational diagnostics.
    const audioAtRaf = outputContextTime(rafMs, stamp);
    const audioAtCallback = outputContextTime(callbackMs, stamp);
    const audioAtQueryEnd = outputContextTime(queryEndMs, stamp);
    const mapping = pair.mappingMs;
    const old = this.previous;
    if (old) {
      const gap = rafMs - old.rafMs;
      if (gap <= 0 || beforeMs(callbackMs, old.queryEndMs) || beforeSeconds(stamp.contextTime, old.contextTime) ||
          beforeMs(queryStartMs, old.queryEndMs) || beforeMs(stamp.performanceTime, old.performanceTime))
        return this.invalidate("clock_moved_backwards");
      // An estimator can regress despite nondecreasing input clocks. Preserve
      // the rejection, but do not label it a raw-clock reversal or bridge fault.
      if (beforeSeconds(audioAtQueryEnd, old.audioAtQueryEnd))
        return this.invalidate("derived_audio_estimate_regressed");
      this.maxFrameGapMs = Math.max(this.maxFrameGapMs, gap);
      const baseline = median(this.initialFrameIntervals);
      const gapLimit = this.initialFrameIntervals.length >= 30
        ? Math.min(POLICY.maxFrameGapMs, baseline * POLICY.adaptiveGapMultiplier + POLICY.adaptiveGapSlackMs)
        : POLICY.maxFrameGapMs;
      if (gap > gapLimit) return this.invalidate("animation_frame_gap");
      if (this.initialFrameIntervals.length < 120) this.initialFrameIntervals.push(gap);
      const step = Math.abs(mapping - old.mapping);
      this.maxMappingStepMs = Math.max(this.maxMappingStepMs, step);
      if (step > POLICY.maxMappingStepMs) return this.invalidate("audio_mapping_jump");
      if (stamp.contextTime > old.contextTime && stamp.performanceTime > old.performanceTime)
        this.lastStampAdvanceMs = queryEndMs;
      if (queryEndMs - this.lastStampAdvanceMs > POLICY.maxTimestampStallMs)
        return this.invalidate("audio_timestamp_stalled");
    } else {
      this.lastStampAdvanceMs = queryEndMs;
      if (audioAtQueryEnd >= this.events[0].audioContextOnset)
        return this.invalidate("first_frame_after_first_event");
    }
    const frame = { sequence: ++this.frameCount, rafMs, callbackMs, callbackEntryMs: callbackMs, queryStartMs, queryEndMs,
      contextTime: stamp.contextTime, performanceTime: stamp.performanceTime,
      audioAtRaf, audioAtCallback, audioAtQueryEnd, mapping, timestampAgeMs: pair.signedAgeMs,
      timestampAheadMs: pair.aheadMs, timestampBehindMs: pair.behindMs, timestampDistanceMs: pair.distanceMs,
      queryDurationMs: query.queryDurationMs, samplingWindowMs: query.samplingWindowMs,
      drawStartMs: null, drawEndMs: null };
    let edge = false;
    for (const event of this.events) {
      if (event.onset === null && audioAtQueryEnd >= event.audioContextOnset) {
        if (audioAtQueryEnd >= event.audioContextEnd) return this.invalidate("missed_event_no_replay");
        event.onset = this.edge(frame, event.audioContextOnset);
        if (event.onset.queryEndMinusEstimatedAudioMs > POLICY.maxEdgeLatenessMs)
          return this.invalidate("late_flash_onset");
        this.activeEvent = event.id;
        edge = true;
      }
      if (event.onset !== null && event.end === null && audioAtQueryEnd >= event.audioContextEnd) {
        event.end = this.edge(frame, event.audioContextEnd);
        if (event.end.queryEndMinusEstimatedAudioMs > POLICY.maxEdgeLatenessMs)
          return this.invalidate("late_flash_end");
        this.activeEvent = null;
        edge = true;
      }
    }
    if (edge && old && this.frames.at(-1)?.sequence !== old.sequence) this.frames.push(old);
    if (edge || this.logNext || rafMs - this.lastLoggedMs >= POLICY.logIntervalMs) {
      this.frames.push(frame);
      this.lastLoggedMs = rafMs;
    }
    this.logNext = edge;
    this.previous = frame;
    if (this.frames.length > POLICY.maxLoggedFrames) {
      this.frames.length = POLICY.maxLoggedFrames;
      return this.invalidate("frame_log_limit");
    }
    if (edge) this.pendingDraw = frame.sequence;
    if (audioAtQueryEnd >= this.startContextTime + DURATION_SECONDS) {
      if (!this.events.every(event => event.onset !== null && event.end !== null))
        return this.invalidate("incomplete_marker_sequence");
      this.status = "completed";
      this.activeEvent = null;
    }
    return this.result();
  }
  edge(frame, contextTime) {
    const estimate = outputPerformanceTime(contextTime, frame);
    return { sequence: frame.sequence, rafMs: frame.rafMs, callbackMs: frame.callbackMs,
      callbackEntryMs: frame.callbackMs, queryStartMs: frame.queryStartMs, queryEndMs: frame.queryEndMs,
      audioContextTime: contextTime, estimatedAudioOutputMs: estimate,
      rafMinusEstimatedAudioMs: frame.rafMs - estimate,
      callbackMinusEstimatedAudioMs: frame.callbackMs - estimate,
      queryEndMinusEstimatedAudioMs: frame.queryEndMs - estimate,
      drawStartMs: null, drawEndMs: null, drawMinusEstimatedAudioMs: null };
  }
  confirmDraw(sequence, startMs, endMs) {
    if (this.status !== "running") return this.result();
    if (sequence !== this.pendingDraw || !finite(startMs) || !finite(endMs) ||
        beforeMs(startMs, this.previous.queryEndMs) || beforeMs(endMs, startMs))
      return this.invalidate("invalid_draw_confirmation");
    this.previous.drawStartMs = startMs;
    this.previous.drawEndMs = endMs;
    for (const event of this.events) for (const edge of [event.onset, event.end]) {
      if (edge?.sequence === sequence) {
        edge.drawStartMs = startMs;
        edge.drawEndMs = endMs;
        edge.drawMinusEstimatedAudioMs = startMs - edge.estimatedAudioOutputMs;
        if (endMs - edge.estimatedAudioOutputMs > POLICY.maxEdgeLatenessMs)
          return this.invalidate("late_draw_submission");
      }
    }
    this.pendingDraw = null;
    return this.result();
  }
  report() {
    return {
      schema: "avsync.browser-reference.v1", runId: this.runId,
      status: this.status, reason: this.reason,
      sourcePolicyVersion: POLICY.version,
      physicalSyncVerified: false,
      interpretation: "Submission timestamps and estimated sink audio timing only; not physical A/V measurement.",
      schedulingClock: "performance.now() sampled after getOutputTimestamp returns; raw rAF/callback entry are observational",
      startContextTime: this.startContextTime, durationSeconds: DURATION_SECONDS,
      sampleRate: this.sampleRate, amplitude: AMPLITUDE, edgeFadeSeconds: EDGE_FADE_SECONDS,
      policy: POLICY, frameCount: this.frameCount, loggedFrameCount: this.frames.length,
      logSampling: "At least every 8 ms, plus before/on/after marker edges; quality checks cover every callback.",
      medianInitialFrameIntervalMs: median(this.initialFrameIntervals),
      maxFrameGapMs: this.maxFrameGapMs, maxTimestampAgeMs: this.maxTimestampAgeMs,
      maxTimestampBehindMs: this.maxTimestampAgeMs, maxTimestampAheadMs: this.maxTimestampAheadMs,
      maxTimestampDistanceMs: this.maxTimestampDistanceMs,
      maxTimestampQueryMs: this.maxTimestampQueryMs,
      maxMappingStepMs: this.maxMappingStepMs, lastObservation: this.lastObservation,
      events: this.events, frames: this.frames,
    };
  }
}
