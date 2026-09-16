// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_diagnostic.hpp"
#include <algorithm>
#include <cmath>

namespace avsync {
bool correction_diagnostic_pass(std::span<const DiagnosticAudioSession> sessions,bool queue_failed,
        std::optional<Nanoseconds> pause) noexcept {
    if (queue_failed || sessions.size()!=(pause ? 2u:1u)) return false;
    for (std::size_t i=0;i<sessions.size();++i) {
        const auto& s=sessions[i];
        if (s.delivered_frames<48000 || s.dispatch_calls_max>8 ||
            s.correction.maximum_predicted_phase_ns>AudioPhaseLedger::phase_limit_ns) return false;
        if (pause && i==0) {
            if (s.state!=CorrectionState::faulted || s.fault!=CorrectionFault::health || !s.first_fault_ns) return false;
            const auto distance=checked_sub(*s.first_fault_ns,*pause);
            if (!distance || *distance<0 || *distance>100'000'000) return false;
        } else if (s.state!=CorrectionState::running || s.fault!=CorrectionFault::none || s.first_fault_ns) return false;
    }
    return true;
}
bool decode_l24(std::span<const std::byte> payload,DiagnosticAudioPacket& packet) noexcept {
    if (!packet.frames || packet.frames>wire::maximum_audio_payload_frames ||
        payload.size()!=packet.frames*6 || !payload.data()) return false;
    for (std::size_t i=0;i<packet.frames*2;++i) {
        auto value=static_cast<std::int32_t>((std::to_integer<std::uint32_t>(payload[i*3])<<16) |
            (std::to_integer<std::uint32_t>(payload[i*3+1])<<8) | std::to_integer<std::uint32_t>(payload[i*3+2]));
        if (value&0x800000) value-=0x1000000;
        packet.pcm[i]=static_cast<float>(value)/8388608.F;
    }
    return true;
}
AudioCorrectionDiagnostic::AudioCorrectionDiagnostic(std::uint64_t clock_epoch)
    : clock_epoch_(clock_epoch),admission_(clock_epoch) {}
bool AudioCorrectionDiagnostic::submit(const DiagnosticAudioPacket& packet) noexcept {
    if (failed_.load()) return false;
    if (!packet.frames || packet.frames>wire::maximum_audio_payload_frames || packet.arrival_ns<0) {
        failed_.store(true); return false;
    }
    std::lock_guard lock(mutex_);
    if (size_==capacity) { failed_.store(true); return false; }
    queue_[(head_+size_)%capacity]=packet; ++size_;
    queue_peak_=std::max(queue_peak_,size_); return true;
}
void AudioCorrectionDiagnostic::snapshot() noexcept {
    if (!worker_) return;
    auto& s=sessions_[session_count_-1];
    s.correction=worker_->diagnostics(); s.state=worker_->state(); s.fault=worker_->fault();
    if (s.state==CorrectionState::faulted && !s.first_fault_ns) s.first_fault_ns=last_tick_;
    s.command_ppm=worker_->command_ppm(); s.target_ppm=worker_->target_ppm();
}
void AudioCorrectionDiagnostic::fail(Nanoseconds now) noexcept {
    failed_.store(true);
    if (worker_) (void)worker_->dispatch(now,false);
    { std::lock_guard lock(mutex_); queue_={}; size_=head_=0; }
    snapshot();
}
void AudioCorrectionDiagnostic::tick(Nanoseconds now,bool provider_healthy) {
    if (failed_.load() || now<0 || (last_tick_ && now<*last_tick_)) { fail(now); return; }
    last_tick_=now;
    // Clock-provider failure closes the old generation immediately, even with
    // no incoming packets. A new authorized generation must re-prime later.
    if (worker_ && !provider_healthy) (void)worker_->dispatch(now,false);
    for (std::size_t count=0;count<capacity;++count) {
        DiagnosticAudioPacket packet;
        {
            std::lock_guard lock(mutex_);
            if (!size_ || queue_[head_].arrival_ns>now) break;
            packet=queue_[head_]; queue_[head_]={}; head_=(head_+1)%capacity; --size_;
        }
        const auto age=checked_sub(now,packet.arrival_ns);
        if (!age || *age>maximum_queue_age_ns) { fail(now); return; }
        if (!admission_.admit(packet.record,packet.ssrc)) continue;
        if (!worker_ || sessions_[session_count_-1].epoch!=packet.record.epoch) {
            snapshot();
            if (session_count_==sessions_.size()) { fail(now); return; }
            worker_=std::make_unique<AudioCorrectionWorker>(packet.record.epoch,clock_epoch_);
            auto& s=sessions_[session_count_++]; s.epoch=packet.record.epoch; s.ssrc=packet.ssrc;
            last_report_=-1;
        }
        auto& s=sessions_[session_count_-1]; ++s.packets;
        last_report_=std::max(last_report_,packet.last_report_ns);
        const bool sr_fresh=last_report_>=0 && last_report_<=now && now-last_report_<=2'000'000'000;
        // Priming discards all PCM. Original frame zero must be observed before
        // RTCP probation completes. Never publish running output without SR health.
        const bool healthy=provider_healthy && (worker_->state()==CorrectionState::priming || sr_fresh);
        const auto pushed=worker_->push(packet.record,packet.ssrc,packet.timestamp,
            std::span(packet.pcm).first(packet.frames*2),now,healthy);
        (void)pushed; // State/fault snapshot retains rejection; no auto-revival.
    }
    if (!worker_) return;
    const bool sr_fresh=last_report_>=0 && last_report_<=now && now-last_report_<=2'000'000'000;
    const bool healthy=provider_healthy && (worker_->state()==CorrectionState::priming || sr_fresh);
    const auto before=worker_->diagnostics().backend_calls;
    (void)worker_->dispatch(now,healthy);
    auto& s=sessions_[session_count_-1]; ++s.dispatches;
    s.dispatch_calls_max=std::max(s.dispatch_calls_max,worker_->diagnostics().backend_calls-before);
    // One pull can take every bounded output quantum; no future scheduling.
    if (const auto block=worker_->pull(output_,now,healthy)) {
        if (block->first_frame!=s.delivered_frames) { fail(now); return; }
        if (!s.delivered_frames) { s.first_frame=block->first_frame; s.first_capture_ns=block->capture_grid_ns; }
        s.last_frame=block->first_frame; s.last_capture_ns=block->capture_grid_ns;
        const auto age=checked_sub(now,block->capture_grid_ns);
        if (!age) { fail(now); return; }
        s.maximum_output_age_ns=std::max(s.maximum_output_age_ns,*age);
        for (std::size_t i=0;i<block->frames*2;++i) {
            const double x=output_[i];
            if (!std::isfinite(x)) { fail(now); return; }
            s.peak=std::max(s.peak,std::abs(x)); s.sum_squares+=x*x;
        }
        s.delivered_frames+=block->frames;
        std::fill(output_.begin(),output_.end(),0);
    }
    snapshot();
}
} // namespace avsync
