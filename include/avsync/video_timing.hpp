// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace avsync {

// The platform adapter normalizes raw driver flags into these labels. A source
// point describes what the driver advertises, not a verified optical event.
enum class TimestampClock { unknown, monotonic, copied };
enum class TimestampPoint { unknown, start_of_exposure, end_of_frame };

// No arrival-time or realtime-clock fallback. Accept only canonical,
// nonnegative timeval fields representable as signed nanoseconds.
[[nodiscard]] constexpr std::optional<std::int64_t> timestamp_from_timeval(
    std::int64_t seconds, std::int64_t micros, TimestampClock clock) noexcept
{
    constexpr std::int64_t ns_per_second = 1'000'000'000;
    constexpr std::int64_t ns_per_microsecond = 1'000;
    constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
    if (clock != TimestampClock::monotonic || seconds < 0 ||
        micros < 0 || micros >= 1'000'000)
        return std::nullopt;
    const auto fractional_ns = micros * ns_per_microsecond;
    if (seconds > (maximum - fractional_ns) / ns_per_second)
        return std::nullopt;
    return seconds * ns_per_second + fractional_ns;
}

enum class VideoSequenceStatus {
    first, normal, gap, repeated, backwards, ambiguous
};
enum class VideoTimestampStatus {
    first, forward, repeated, regression, invalid
};

struct VideoTimingObservation {
    VideoSequenceStatus sequence_status = VideoSequenceStatus::first;
    VideoTimestampStatus timestamp_status = VideoTimestampStatus::invalid;
    // A modular forward gap's missing sequence values, not proof that the
    // source image changed. Count as accepted loss only when accepted() is true.
    std::uint32_t missing_frames = 0;

    [[nodiscard]] constexpr bool accepted() const noexcept
    {
        return (sequence_status == VideoSequenceStatus::first ||
                sequence_status == VideoSequenceStatus::normal ||
                sequence_status == VideoSequenceStatus::gap) &&
               (timestamp_status == VideoTimestampStatus::first ||
                timestamp_status == VideoTimestampStatus::forward);
    }
};

// One instance per progressive capture generation. Unsigned modular distance
// handles 32-bit wrap; half-range jumps are ambiguous and larger jumps backwards.
// Rejected observations never move the anchor, so a bad packet cannot silently
// rebase time or turn a replay into a new stream. The owner may instead stop and
// start an explicit new generation. This policy is not for alternating fields.
class VideoTimingTracker {
public:
    [[nodiscard]] constexpr VideoTimingObservation observe(
        std::uint32_t sequence, std::int64_t timestamp_ns) noexcept
    {
        VideoTimingObservation result;
        if (!previous_) {
            if (timestamp_ns < 0)
                return result;
            result.timestamp_status = VideoTimestampStatus::first;
            previous_ = Point{sequence, timestamp_ns};
            return result;
        }

        const auto distance = static_cast<std::uint32_t>(sequence - previous_->sequence);
        if (distance == 0) {
            result.sequence_status = VideoSequenceStatus::repeated;
        } else if (distance == 1) {
            result.sequence_status = VideoSequenceStatus::normal;
        } else if (distance < 0x8000'0000U) {
            result.sequence_status = VideoSequenceStatus::gap;
            result.missing_frames = distance - 1;
        } else if (distance == 0x8000'0000U) {
            result.sequence_status = VideoSequenceStatus::ambiguous;
        } else {
            result.sequence_status = VideoSequenceStatus::backwards;
        }

        if (timestamp_ns < 0)
            result.timestamp_status = VideoTimestampStatus::invalid;
        else if (timestamp_ns < previous_->timestamp_ns)
            result.timestamp_status = VideoTimestampStatus::regression;
        else if (timestamp_ns == previous_->timestamp_ns)
            result.timestamp_status = VideoTimestampStatus::repeated;
        else
            result.timestamp_status = VideoTimestampStatus::forward;

        if (result.accepted())
            previous_ = Point{sequence, timestamp_ns};
        return result;
    }

private:
    struct Point {
        std::uint32_t sequence;
        std::int64_t timestamp_ns;
    };
    std::optional<Point> previous_;
};

} // namespace avsync
