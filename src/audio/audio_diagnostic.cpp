// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_diagnostic.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace avsync {
bool desktop_fault_recoverable(CorrectionFault fault,CorrectionStaleReason stale) noexcept {
    return fault==CorrectionFault::health || (fault==CorrectionFault::stale &&
        (stale==CorrectionStaleReason::no_progress || stale==CorrectionStaleReason::anchor_age));
}
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
AudioCorrectionDiagnostic::AudioCorrectionDiagnostic(std::uint64_t clock_epoch,DiagnosticAudioOutput output,
        DesktopRecovery recovery)
    : destination_(output),recovery_(recovery),clock_epoch_(clock_epoch),admission_(clock_epoch) {
    if ((destination_.consume!=nullptr)!=(destination_.revoke!=nullptr))
        throw std::invalid_argument("output requires consume and revoke hooks");
    if (recovery_!=DesktopRecovery::disabled &&
        (recovery_!=DesktopRecovery::same_sender_session || !destination_.consume || !destination_.begin_generation))
        throw std::invalid_argument("same-session recovery requires complete output generation hooks");
}
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
bool AudioCorrectionDiagnostic::retired_generation(SessionToken epoch) const noexcept {
    return epoch.valid() && epoch.session==admitted_session_.load(std::memory_order_acquire) &&
        epoch.generation<=retired_generation_.load(std::memory_order_acquire);
}
bool AudioCorrectionDiagnostic::retire_stale_transport(SessionToken epoch) noexcept {
    if (recovery_!=DesktopRecovery::same_sender_session || !epoch.valid() ||
        epoch.session!=admitted_session_.load(std::memory_order_acquire) ||
        epoch.generation>admitted_generation_.load(std::memory_order_acquire)) return false;
    auto retired=retired_generation_.load(std::memory_order_relaxed);
    while (retired<epoch.generation && !retired_generation_.compare_exchange_weak(
        retired,epoch.generation,std::memory_order_release,std::memory_order_relaxed)) {}
    return true;
}
void AudioCorrectionDiagnostic::apply_retirement(Nanoseconds now) noexcept {
    if (worker_ && worker_->state()!=CorrectionState::faulted &&
        retired_generation(sessions_[session_count_-1].epoch)) {
        (void)worker_->dispatch(now,false); snapshot();
    }
}
void AudioCorrectionDiagnostic::snapshot() noexcept {
    if (!worker_) return;
    auto& s=sessions_[session_count_-1];
    s.correction=worker_->diagnostics(); s.state=worker_->state(); s.fault=worker_->fault();
    if (s.state==CorrectionState::faulted && destination_.revoke) {
        if (!output_revoked_) { destination_.revoke(destination_.context); output_revoked_=true; }
        const bool recoverable=recovery_==DesktopRecovery::same_sender_session &&
            desktop_fault_recoverable(s.fault,s.correction.stale_reason);
        if (recoverable) (void)retire_stale_transport(s.epoch);
        awaiting_generation_=recoverable && !failed_.load();
        if (!recoverable) failed_.store(true);
    }
    if (s.state==CorrectionState::faulted && !s.first_fault_ns) s.first_fault_ns=last_tick_;
    s.command_ppm=worker_->command_ppm(); s.target_ppm=worker_->target_ppm();
}
void AudioCorrectionDiagnostic::fail(Nanoseconds now) noexcept {
    failed_.store(true);
    awaiting_generation_=false;
    if (destination_.revoke && !output_revoked_) { destination_.revoke(destination_.context); output_revoked_=true; }
    if (worker_) (void)worker_->dispatch(now,false);
    { std::lock_guard lock(mutex_); queue_={}; size_=head_=0; }
    snapshot();
}
void AudioCorrectionDiagnostic::tick(Nanoseconds now,bool provider_healthy) {
    if (failed_.load() || now<0 || (last_tick_ && now<*last_tick_)) { fail(now); return; }
    last_tick_=now;
    apply_retirement(now);
    // Clock-provider failure closes the old generation immediately, even with
    // no incoming packets. A new authorized generation must re-prime later.
    if (worker_ && !provider_healthy) { (void)worker_->dispatch(now,false); snapshot(); }
    for (std::size_t count=0;count<capacity;++count) {
        DiagnosticAudioPacket packet;
        {
            std::lock_guard lock(mutex_);
            if (!size_ || queue_[head_].arrival_ns>now) break;
            packet=queue_[head_]; queue_[head_]={}; head_=(head_+1)%capacity; --size_;
        }
        if (retired_generation(packet.record.epoch)) { ++retired_packets_; continue; }
        const auto age=checked_sub(now,packet.arrival_ns);
        if (!age || *age>maximum_queue_age_ns) { fail(now); return; }
        const auto active=admission_.active_epoch();
        if (recovery_==DesktopRecovery::same_sender_session && active &&
            packet.record.epoch.session!=active->session) { fail(now); return; }
        if (active && packet.record.epoch.session==active->session &&
            packet.record.epoch.generation<active->generation) { ++retired_packets_; continue; }
        if (!admission_.admit(packet.record,packet.ssrc)) {
            if (recovery_==DesktopRecovery::same_sender_session) { fail(now); return; }
            continue;
        }
        if (!worker_ || sessions_[session_count_-1].epoch!=packet.record.epoch) {
            if (worker_ && destination_.revoke) {
                if (recovery_==DesktopRecovery::disabled) { fail(now); return; }
                // A valid successor may arrive before the old watchdog fires.
                // Retire all old DSP/IPC before preparing the new generation.
                if (worker_->state()!=CorrectionState::faulted) (void)worker_->dispatch(now,false);
            }
            snapshot();
            if (failed_.load()) return;
            if (session_count_==sessions_.size()) { fail(now); return; }
            if (destination_.begin_generation && !destination_.begin_generation(destination_.context,packet.record.epoch)) {
                fail(now); return;
            }
            output_revoked_=false; awaiting_generation_=false;
            worker_=std::make_unique<AudioCorrectionWorker>(packet.record.epoch,clock_epoch_);
            auto& s=sessions_[session_count_++]; s.epoch=packet.record.epoch; s.ssrc=packet.ssrc;
            admitted_session_.store(packet.record.epoch.session,std::memory_order_release);
            admitted_generation_.store(packet.record.epoch.generation,std::memory_order_release);
            last_report_=-1;
        }
        if (worker_->state()==CorrectionState::faulted && recovery_==DesktopRecovery::same_sender_session) {
            ++retired_packets_; continue; // Never revive a failed generation with new arrivals.
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
        snapshot();
        if (failed_.load()) return;
    }
    if (!worker_) return;
    apply_retirement(now);
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
        if (destination_.consume && !destination_.consume(destination_.context,*block,
                std::span(output_).first(block->frames*2),now)) { fail(now); return; }
        std::fill(output_.begin(),output_.end(),0);
    }
    snapshot();
}
} // namespace avsync
