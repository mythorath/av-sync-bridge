// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/timing.hpp"
#include "avsync/video_handoff.hpp"

namespace avsync {

enum class VideoPublicationResult { empty, published, busy, stale, invalid, failed };

inline constexpr Nanoseconds maximum_video_publication_age_ns = 200'000'000;
inline constexpr Nanoseconds maximum_video_capture_future_ns = 1'000'000;

// Consumer-thread helper: exactly one nonblocking publication attempt per call.
// The callback must return published, busy, or failed, and must not retain its
// borrowed span. Busy retains the SAME owned queue slot for a later worker tick;
// neither its pixels nor its original capture/presentation timestamps change.
// The existing two-slot queue bounds memory and keeps the capture thread free to
// reject a new frame if full. Old frames expire by capture age, never retry age.
// Invalid/failed leaves the packet owned until the caller tears down the epoch;
// callers must not continue after those results. No allocation or waiting here.
template<class Publish>
[[nodiscard]] VideoPublicationResult try_publish_oldest_video(
    Nv12VideoHandoff& handoff, Nanoseconds now, Nanoseconds delay, Publish&& publish)
{
    if (now < 0 || delay < 0)
        return VideoPublicationResult::invalid;
    const auto* packet = handoff.peek();
    if (!packet)
        return VideoPublicationResult::empty;
    const auto presentation = checked_add(packet->capture_ns, delay);
    const auto age = checked_sub(now, packet->capture_ns);
    if (!presentation || !age || *age < -maximum_video_capture_future_ns)
        return VideoPublicationResult::invalid;
    if (*age > maximum_video_publication_age_ns)
        return handoff.release() ? VideoPublicationResult::stale : VideoPublicationResult::failed;
    const auto result = publish(std::span<const std::uint8_t>(packet->pixels),
                                packet->capture_ns, *presentation);
    if (result == VideoPublicationResult::published)
        return handoff.release() ? result : VideoPublicationResult::failed;
    return result == VideoPublicationResult::busy ? result : VideoPublicationResult::failed;
}

} // namespace avsync
