// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_handoff.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace avsync {
CorrectedAudioHandoff::CorrectedAudioHandoff(std::string path,Nanoseconds delay_ns,
        ipc::ReplacementPolicy replacement):delay_(delay_ns) {
    if (delay_<100'000'000 || delay_>2'000'000'000 || delay_%10'000'000)
        throw std::invalid_argument("desktop handoff delay must be 100..2000 ms in 10 ms steps");
    ipc::Config config;
    config.width=config.height=2; config.video_capacity=2; // Unused video slots.
    config.audio_capacity=static_cast<std::uint32_t>(delay_/10'000'000+24);
    writer_=std::make_unique<ipc::Writer>(std::move(path),config,ipc::AllocationPolicy::reserve_and_prefault,replacement);
    if (!writer_->valid()) throw std::runtime_error(writer_->error());
}
void CorrectedAudioHandoff::revoke() noexcept { writer_.reset(); pending_={}; size_=head_=0; }
bool CorrectedAudioHandoff::flush(Nanoseconds now) noexcept {
    if (!valid()) return false;
    for (unsigned count=0;count<8 && size_;++count) {
        const auto& p=pending_[head_];
        const auto age=checked_sub(now,p.capture);
        if (!age || *age>200'000'000 || *age < -100'000'000 || now>p.presentation) {
            failure_="retry_deadline"; revoke(); return false;
        }
        const auto result=writer_->try_publish_audio(0,p.pcm,p.capture,p.presentation);
        if (result==ipc::WriteResult::busy) { ++busy_; return true; }
        if (result!=ipc::WriteResult::ok) { failure_="ipc_write_rejected"; revoke(); return false; }
        pending_[head_]={}; head_=(head_+1)%pending_.size(); --size_; frames_+=480;
    }
    return true;
}
bool CorrectedAudioHandoff::heartbeat(Nanoseconds now) noexcept {
    if (now<0) { failure_="invalid_time"; revoke(); return false; }
    if (!flush(now)) return false;
    const auto result=writer_->try_heartbeat();
    if (result==ipc::WriteResult::ok || result==ipc::WriteResult::busy) return true;
    failure_="ipc_heartbeat_rejected"; revoke(); return false;
}
bool CorrectedAudioHandoff::consume(const CorrectedAudio& block,std::span<const float> pcm,Nanoseconds now) noexcept {
    const auto reject=[&] { failure_="invalid_block_timestamp_or_capacity"; revoke(); return false; };
    if (!valid() || !block.epoch.valid() || !block.frames || block.frames>AudioCorrectionWorker::output_capacity ||
        block.frames%480 || pcm.size()!=block.frames*2 || !pcm.data() || block.first_frame!=accepted_ ||
        now<0 || block.capture_grid_ns<0 || !std::all_of(pcm.begin(),pcm.end(),[](float x){return std::isfinite(x);}))
        return reject();
    const auto age=checked_sub(now,block.capture_grid_ns);
    if (!age || *age < -100'000'000 || *age>200'000'000) return reject();
    if (!epoch_) { epoch_=block.epoch; timeline_.emplace(block.capture_grid_ns,48000); }
    if (*epoch_!=block.epoch || timeline_->at(accepted_)!=block.capture_grid_ns ||
        accepted_>std::numeric_limits<std::uint64_t>::max()-block.frames ||
        block.frames/480>pending_.size()-size_) return reject();
    for (std::size_t offset=0;offset<block.frames;offset+=480) {
        const auto capture=timeline_->at(accepted_+offset);
        const auto presentation=capture ? checked_add(*capture,delay_):std::nullopt;
        if (!presentation || *presentation<now) return reject();
        auto& pending=pending_[(head_+size_)%pending_.size()];
        pending.capture=*capture; pending.presentation=*presentation;
        std::copy_n(pcm.begin()+offset*2,960,pending.pcm.begin()); ++size_;
        peak_=std::max(peak_,size_);
    }
    accepted_+=block.frames; return flush(now);
}
} // namespace avsync
