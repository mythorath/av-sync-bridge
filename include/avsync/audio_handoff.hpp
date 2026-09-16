// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "avsync/audio_correction.hpp"
#include "avsync/ipc.hpp"
#include <memory>

namespace avsync {
// Linux, desktop-only, one media generation per explicitly started instance.
// Owner/control thread only: construction and revoke perform filesystem/mutex
// teardown. NEVER call from an OBS or capture callback. Steady writes trylock.
// No source is automatically added to OBS; no microphone/video is published.
class CorrectedAudioHandoff {
public:
    // Process replacement is explicit; see IPC's strict predecessor-retirement
    // contract. Default same-owner diagnostic behavior remains unchanged.
    CorrectedAudioHandoff(std::string path, Nanoseconds delay_ns,
        ipc::ReplacementPolicy replacement = ipc::ReplacementPolicy::atomic_replace);
    bool consume(const CorrectedAudio&, std::span<const float>, Nanoseconds now) noexcept;
    void revoke() noexcept;
    bool heartbeat(Nanoseconds now) noexcept;
    [[nodiscard]] bool valid() const noexcept { return writer_ && writer_->valid(); }
    [[nodiscard]] std::uint64_t published_frames() const noexcept { return frames_; }
    [[nodiscard]] std::uint64_t busy_retries() const noexcept { return busy_; }
    [[nodiscard]] std::size_t queue_peak() const noexcept { return peak_; }
    [[nodiscard]] const char* failure_reason() const noexcept { return failure_; }
private:
    bool flush(Nanoseconds now) noexcept;
    struct Pending { std::array<float,960> pcm{}; Nanoseconds capture{},presentation{}; };
    std::array<Pending,16> pending_{};
    std::size_t head_{},size_{},peak_{};
    std::unique_ptr<ipc::Writer> writer_;
    Nanoseconds delay_;
    std::optional<SessionToken> epoch_;
    std::optional<RationalTimeline> timeline_;
    std::uint64_t frames_{},accepted_{},busy_{};
    const char* failure_{"none"};
};
} // namespace avsync
