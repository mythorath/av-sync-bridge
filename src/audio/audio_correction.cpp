// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_correction.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace avsync {
AudioCorrectionWorker::AudioCorrectionWorker(SessionToken epoch, std::uint64_t clock_epoch)
    : epoch_(epoch), clock_epoch_(clock_epoch), validator_(clock_epoch) {
    if (!epoch.valid()) throw std::invalid_argument("invalid correction epoch");
    if (!SincPhaseModel::supports(AsrcStereo::backend_version()))
        throw std::runtime_error("correction phase model requires audited libsamplerate 0.2.2");
}
void AudioCorrectionWorker::fail(CorrectionFault reason) noexcept {
    if (state_ == CorrectionState::faulted) return;
    state_ = CorrectionState::faulted; fault_ = reason;
    input_head_ = input_size_ = output_head_ = output_size_ = 0;
    std::fill(input_.begin(), input_.end(), 0);
    std::fill(output_.begin(), output_.end(), 0);
    std::fill(scratch_input_.begin(), scratch_input_.end(), 0);
    std::fill(scratch_output_.begin(), scratch_output_.end(), 0);
    (void)backend_.reset(); // No EOS drain; failed worker cannot expose media.
    phase_ledger_.clear();
    phase_model_ = {};
    std::fill(phase_positions_.begin(), phase_positions_.end(), PredictedSourcePosition{});
}
bool AudioCorrectionWorker::check_health(Nanoseconds now, bool healthy) noexcept {
    if (state_ == CorrectionState::faulted) return false;
    if (!healthy || now < 0 || (last_now_ && now < *last_now_)) {
        fail(CorrectionFault::health); return false;
    }
    last_now_ = now;
    // A drained public queue does not mean the backend has no private history.
    // An explicit no-progress watchdog forbids resuming that history after a stall.
    if (last_progress_) {
        const auto age = checked_sub(now, *last_progress_);
        if (!age || *age > queue_lifetime_ns) { fail(CorrectionFault::stale); return false; }
    }
    if (const auto& record = validator_.latest_record()) {
        const auto age = checked_sub(now, record->capture_ns);
        if (!age || *age > 250'000'000 || *age < -100'000'000) {
            fail(CorrectionFault::stale); return false;
        }
    }
    for (const auto arrival : {input_size_ ? input_arrivals_[input_head_] : now,
                               output_size_ ? output_arrivals_[output_head_] : now}) {
        const auto age = checked_sub(now, arrival);
        if (!age || *age < 0 || *age > queue_lifetime_ns) {
            fail(CorrectionFault::stale); return false;
        }
    }
    return true;
}
bool AudioCorrectionWorker::start(const wire::AudioRecord& record) noexcept {
    const auto anchor = nominal_wire_position(record.device_position, record.device_origin, 0, record.source_rate);
    if (!anchor || anchor->whole > record.packet_wire_start) return false;
    const long double delta = static_cast<long double>(record.packet_wire_start - anchor->whole) -
        static_cast<long double>(anchor->remainder) / anchor->denominator;
    // A fresh original anchor, not arrival, establishes the post-acquisition
    // segment. Bound this rate-based extrapolation to 50ms nominal media.
    if (delta < 0 || delta > 2'400) return false;
    const auto ns = std::floor(delta * 1e9L / (48'000.L * (1 + target_ / 1e6L)));
    if (!std::isfinite(ns) || ns < 0 || ns > 100'000'000) return false;
    origin_ = checked_add(record.capture_ns, static_cast<Nanoseconds>(ns));
    if (!origin_ || !backend_.reset(target_)) return false;
    timeline_.emplace(*origin_, 48'000);
    origin_wire_ = record.packet_wire_start; command_ = target_; last_progress_ = last_now_;
    if (!phase_model_.reset(origin_wire_, target_)) return false;
    state_ = CorrectionState::running;
    return true;
}
CorrectionPush AudioCorrectionWorker::push(const wire::AudioRecord& record, std::uint32_t ssrc,
    std::uint32_t timestamp, std::span<const float> stereo, Nanoseconds now, bool healthy) noexcept {
    if (record.epoch != epoch_) return CorrectionPush::wrong_epoch;
    if (!check_health(now, healthy)) return CorrectionPush::rejected;
    if (stereo.empty() || stereo.size() % 2 || stereo.size() > 2 * wire::maximum_audio_payload_frames ||
        !stereo.data() || !std::all_of(stereo.begin(), stereo.end(), [](float x) { return std::isfinite(x); })) {
        fail(CorrectionFault::pcm); return CorrectionPush::rejected;
    }
    const auto frames = stereo.size() / 2;
    const auto result = validator_.observe(record, ssrc, timestamp, static_cast<std::uint32_t>(frames), now);
    if (!result.accepted || !ssrc) { fail(CorrectionFault::metadata); return CorrectionPush::rejected; }
    diagnostics_.received_frames += frames;
    if (result.estimate) {
        ++diagnostics_.estimator_windows;
        const double ppm = result.estimate->source_rate_error_ppm;
        if (!result.estimate->within_correction_limit || !AsrcStereo::valid_ppm(ppm)) {
            fail(CorrectionFault::rate); return CorrectionPush::rejected;
        }
        target_ = ppm;
        if (state_ == CorrectionState::priming) {
            acquisition_[acquisition_count_++] = ppm;
        }
    }
    if (state_ == CorrectionState::priming && acquisition_count_ == acquisition_.size()) {
        // Three valid ORIGINAL-clock windows give a bounded initial estimate.
        // Do not restart indefinitely on noisy short-window slopes. This is
        // provisional acquisition, NOT proof of steady lock: every later sample
        // must still pass the original-anchor phase guard, and rate/health limits
        // and command slew remain unchanged. No old priming PCM is retained.
        auto sorted = acquisition_; std::sort(sorted.begin(), sorted.end()); target_ = sorted[1];
        diagnostics_.acquisition_spread_ppm = sorted.back()-sorted.front();
        if (!start(record)) { fail(CorrectionFault::timeline); return CorrectionPush::rejected; }
    }
    if (state_ == CorrectionState::priming) {
        diagnostics_.priming_discarded_frames += frames; return CorrectionPush::priming_discard;
    }
    if (!phase_ledger_.size() || !result.repeated) {
        const auto anchor = nominal_wire_position(record.device_position, record.device_origin, 0, record.source_rate);
        if (!anchor || phase_ledger_.add({anchor->whole, static_cast<double>(anchor->remainder)/anchor->denominator},
            record.capture_ns, phase_model_.next()) != PhaseStatus::ready) {
            fail(CorrectionFault::phase); return CorrectionPush::rejected;
        }
        diagnostics_.peak_phase_anchors = std::max(diagnostics_.peak_phase_anchors, phase_ledger_.size());
    }
    if (frames > input_capacity - input_size_) { fail(CorrectionFault::overflow); return CorrectionPush::rejected; }
    for (std::size_t i = 0; i < frames; ++i) {
        const auto at = (input_head_ + input_size_ + i) % input_capacity;
        input_[2 * at] = stereo[2 * i]; input_[2 * at + 1] = stereo[2 * i + 1];
        input_arrivals_[at] = now;
    }
    input_size_ += frames;
    diagnostics_.peak_input_frames = std::max(diagnostics_.peak_input_frames, input_size_);
    return CorrectionPush::accepted;
}
std::size_t AudioCorrectionWorker::dispatch(Nanoseconds now, bool healthy) noexcept {
    if (!check_health(now, healthy) || state_ != CorrectionState::running) return 0;
    std::size_t produced{};
    for (std::size_t call = 0; call < max_calls && input_size_ >= input_offer &&
            output_capacity - output_size_ >= quantum; ++call) {
        for (std::size_t i = 0; i < input_offer; ++i) {
            const auto at = (input_head_ + i) % input_capacity;
            scratch_input_[2 * i] = input_[2 * at]; scratch_input_[2 * i + 1] = input_[2 * at + 1];
        }
        const double next = command_ + std::clamp(target_ - command_, -command_step_ppm, command_step_ppm);
        auto prediction = phase_model_;
        if (!prediction.advance(next, phase_positions_)) { fail(CorrectionFault::phase); return 0; }
        const auto phase = phase_ledger_.assess(phase_positions_, *origin_, diagnostics_.produced_frames);
        if (phase.status == PhaseStatus::waiting) {
            if (diagnostics_.phase_waits != std::numeric_limits<std::uint64_t>::max()) ++diagnostics_.phase_waits;
            break; // Await an ORIGINAL bracketing anchor, without touching DSP.
        }
        diagnostics_.maximum_predicted_phase_ns = std::max(diagnostics_.maximum_predicted_phase_ns, phase.maximum_absolute_ns);
        if (phase.status != PhaseStatus::ready) { fail(CorrectionFault::phase); return 0; }
        const auto result = backend_.process(scratch_input_, scratch_output_, next);
        ++diagnostics_.backend_calls;
        if (result.status != AsrcStatus::progress || result.output_frames_generated != quantum ||
            result.input_frames_used > input_offer) {
            fail(CorrectionFault::backend); return 0;
        }
        // No partially filled output quantum is exposed. Do not equate input
        // consumption with contributing sample provenance or filter delay.
        if (diagnostics_.produced_frames > std::numeric_limits<std::uint64_t>::max() - quantum ||
            !timeline_->at(diagnostics_.produced_frames + quantum)) {
            fail(CorrectionFault::timeline); return 0;
        }
        diagnostics_.maximum_command_step_ppm = std::max(diagnostics_.maximum_command_step_ppm, std::abs(next - command_));
        command_ = next;
        phase_model_ = prediction;
        diagnostics_.phase_checks += quantum;
        input_head_ = (input_head_ + result.input_frames_used) % input_capacity;
        input_size_ -= result.input_frames_used;
        diagnostics_.consumed_frames += result.input_frames_used;
        for (std::size_t i = 0; i < quantum; ++i) {
            const auto at = (output_head_ + output_size_ + i) % output_capacity;
            output_[2 * at] = scratch_output_[2 * i]; output_[2 * at + 1] = scratch_output_[2 * i + 1];
            output_arrivals_[at] = now;
        }
        output_size_ += quantum; produced += quantum; diagnostics_.produced_frames += quantum;
        last_progress_ = now;
        diagnostics_.peak_output_frames = std::max(diagnostics_.peak_output_frames, output_size_);
    }
    if (!produced && diagnostics_.no_progress_dispatches != std::numeric_limits<std::uint64_t>::max())
        ++diagnostics_.no_progress_dispatches;
    return produced;
}
std::optional<CorrectedAudio> AudioCorrectionWorker::pull(std::span<float> stereo, Nanoseconds now, bool healthy) noexcept {
    if (!check_health(now, healthy) || state_ != CorrectionState::running) return {};
    if (stereo.empty() || stereo.size() % 2 || stereo.size() > output_capacity * 2 || !stereo.data()) {
        fail(CorrectionFault::pcm); return {};
    }
    if (!output_size_) return {};
    const auto count = std::min(output_size_, stereo.size() / 2);
    const auto stamp = timeline_->at(output_index_);
    if (!stamp) { fail(CorrectionFault::timeline); return {}; }
    for (std::size_t i = 0; i < count; ++i) {
        const auto at = (output_head_ + i) % output_capacity;
        stereo[2 * i] = output_[2 * at]; stereo[2 * i + 1] = output_[2 * at + 1];
    }
    const CorrectedAudio result{epoch_, output_index_, *stamp, count};
    output_index_ += count; diagnostics_.delivered_frames += count;
    output_head_ = (output_head_ + count) % output_capacity; output_size_ -= count;
    return result;
}
bool AudioCorrectionWorker::reset(SessionToken next) noexcept {
    if (!valid_successor(epoch_, next)) return false;
    if (!backend_.reset()) { fail(CorrectionFault::backend); return false; }
    epoch_ = next; validator_ = wire::AudioReceiverValidator(clock_epoch_);
    state_ = CorrectionState::priming; fault_ = CorrectionFault::none; diagnostics_ = {};
    acquisition_count_ = 0; target_ = command_ = 0; origin_.reset(); last_now_.reset(); last_progress_.reset(); timeline_.reset();
    phase_ledger_.clear(); phase_model_ = {};
    std::fill(phase_positions_.begin(), phase_positions_.end(), PredictedSourcePosition{});
    origin_wire_ = output_index_ = input_head_ = input_size_ = output_head_ = output_size_ = 0;
    std::fill(input_.begin(), input_.end(), 0); std::fill(output_.begin(), output_.end(), 0);
    std::fill(scratch_input_.begin(), scratch_input_.end(), 0); std::fill(scratch_output_.begin(), scratch_output_.end(), 0);
    return true;
}
} // namespace avsync
