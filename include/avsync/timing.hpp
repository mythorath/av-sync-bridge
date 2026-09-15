// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <deque>
#include <optional>
#include <vector>

namespace avsync {

using Nanoseconds = std::int64_t;

[[nodiscard]] std::optional<Nanoseconds> checked_add(Nanoseconds a, Nanoseconds b) noexcept;
[[nodiscard]] std::optional<Nanoseconds> checked_sub(Nanoseconds a, Nanoseconds b) noexcept;

struct SessionToken {
    std::uint64_t session = 0;
    std::uint64_t generation = 0;
    [[nodiscard]] bool valid() const noexcept { return session != 0 && generation != 0; }
    friend bool operator==(const SessionToken&, const SessionToken&) = default;
};

// A controller must generate unique session IDs; generations strictly increase
// within a session. A generation change is an explicit discontinuity.
[[nodiscard]] bool valid_successor(SessionToken current, SessionToken next) noexcept;

class CaptureToPresentation {
public:
    CaptureToPresentation(SessionToken epoch, Nanoseconds total_delay, Nanoseconds path_offset = 0);
    [[nodiscard]] std::optional<Nanoseconds> map(SessionToken epoch, Nanoseconds capture) const noexcept;
    [[nodiscard]] SessionToken epoch() const noexcept { return epoch_; }
private:
    SessionToken epoch_;
    Nanoseconds offset_;
};

// Exact floor(index * 1e9 * rate_denominator / rate_numerator) + origin.
// No repeated rounded frame/sample increments and no compiler-specific integers.
class RationalTimeline {
public:
    RationalTimeline(Nanoseconds origin, std::uint32_t rate_numerator, std::uint32_t rate_denominator = 1);
    [[nodiscard]] std::optional<Nanoseconds> at(std::uint64_t index) const noexcept;
private:
    Nanoseconds origin_;
    std::uint32_t numerator_;
    std::uint32_t denominator_;
};

struct TimedMetadata {
    SessionToken epoch;
    std::uint64_t sequence = 0;
    Nanoseconds capture_time = 0;
    Nanoseconds presentation_time = 0;
    Nanoseconds duration = 0;
    std::uint64_t payload_bytes = 0;
};

struct QueueLimits {
    std::size_t max_count = 256;
    std::uint64_t max_bytes = 64 * 1024 * 1024;
    Nanoseconds max_future = 3'000'000'000;
    Nanoseconds max_span = 3'000'000'000;
};

enum class PushStatus { accepted, wrong_epoch, invalid, late, too_far_future, full, duplicate };

struct DueResult {
    std::optional<TimedMetadata> item;
    // Return expired identities so a payload owner can release their storage.
    std::vector<TimedMetadata> discarded_late;
};

// Metadata only: the owner must release the corresponding payload on every
// rejection, expiry, or reset. No capacity is learned from startup frame count.
class BoundedTimelineQueue {
public:
    explicit BoundedTimelineQueue(SessionToken epoch, QueueLimits limits = {});
    [[nodiscard]] PushStatus push(TimedMetadata item, Nanoseconds now);
    [[nodiscard]] DueResult pop_due(Nanoseconds now, Nanoseconds allowed_lateness = 0);
    // Invalid/reused generations do not clear the current queue.
    [[nodiscard]] bool reset(SessionToken next) noexcept;
    [[nodiscard]] std::size_t size() const noexcept { return queue_.size(); }
    [[nodiscard]] std::uint64_t bytes() const noexcept { return bytes_; }
    [[nodiscard]] SessionToken epoch() const noexcept { return epoch_; }
private:
    SessionToken epoch_;
    QueueLimits limits_;
    std::deque<TimedMetadata> queue_;
    std::uint64_t bytes_ = 0;
};

struct RateEstimatorConfig {
    std::uint32_t nominal_rate = 48'000;
    double max_correction_ppm = 500.0;
    Nanoseconds min_window = 1'000'000'000;
    Nanoseconds max_observation_gap = 2'000'000'000;
};

struct RateEstimate {
    double measured_rate_hz = 0;
    // Positive means the input sample clock is faster than the shared clock.
    double source_rate_error_ppm = 0;
    double bounded_rate_error_ppm = 0;
    double output_per_input_ratio = 1;
    bool within_correction_limit = false;
};

enum class RateStatus { priming, waiting, measured, discontinuity, wrong_epoch };
struct RateObservation {
    RateStatus status;
    std::optional<RateEstimate> estimate;
};

// One instance per device/stream. Use device sample position paired with actual
// capture time in the shared clock, NEVER packet arrival time. This recommends
// a bounded ratio; it neither resamples nor implements an ASRC feedback loop.
class SampleRateEstimator {
public:
    explicit SampleRateEstimator(SessionToken epoch, RateEstimatorConfig config = {});
    [[nodiscard]] RateObservation observe(SessionToken epoch, std::uint64_t sample_position, Nanoseconds capture_time) noexcept;
    [[nodiscard]] bool reset(SessionToken next) noexcept;
private:
    struct Point { std::uint64_t sample; Nanoseconds time; };
    SessionToken epoch_;
    RateEstimatorConfig config_;
    std::optional<Point> anchor_;
    std::optional<Point> last_;
};

// Check at actual output delivery, not just enqueue. Starts/restarts muted.
// An unmute establishes a capture-time cutoff: buffered earlier voice, including
// blocks straddling the cutoff, is rejected rather than replayed.
class MicPrivacyGate {
public:
    explicit MicPrivacyGate(SessionToken epoch);
    [[nodiscard]] bool set_muted(bool muted, Nanoseconds control_time) noexcept;
    [[nodiscard]] bool permits(SessionToken epoch, Nanoseconds capture_start, Nanoseconds duration) const noexcept;
    [[nodiscard]] bool reset(SessionToken next) noexcept;
    [[nodiscard]] bool muted() const noexcept { return muted_; }
private:
    SessionToken epoch_;
    bool muted_ = true;
    std::optional<Nanoseconds> cutoff_;
    std::optional<Nanoseconds> last_control_;
};

} // namespace avsync
