// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/timing.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace avsync {
namespace {
constexpr auto ns_min = std::numeric_limits<Nanoseconds>::min();
constexpr auto ns_max = std::numeric_limits<Nanoseconds>::max();
constexpr auto u64_max = std::numeric_limits<std::uint64_t>::max();

// Multiply/divide using quotient/remainder pairs. Denominator is a uint32_t,
// so sums of two remainders cannot overflow uint64_t. No intermediate product
// overflow, floating point, or nonportable __int128 is needed.
std::optional<std::uint64_t> mul_div(std::uint64_t n, std::uint64_t units, std::uint32_t denominator) noexcept
{
    std::uint64_t q = units / denominator, r = units % denominator;
    std::uint64_t out_q = 0, out_r = 0;
    while (n != 0) {
        if ((n & 1U) != 0) {
            const auto carry = (out_r + r) / denominator;
            out_r = (out_r + r) % denominator;
            if (q > u64_max - out_q) return std::nullopt;
            out_q += q;
            if (carry > u64_max - out_q) return std::nullopt;
            out_q += carry;
        }
        n >>= 1U;
        if (n != 0) {
            const auto carry = (r + r) / denominator;
            r = (r + r) % denominator;
            if (q > (u64_max - carry) / 2U) return std::nullopt;
            q = q * 2U + carry;
        }
    }
    return out_q;
}

std::optional<Nanoseconds> add_unsigned(Nanoseconds origin, std::uint64_t delta) noexcept
{
    if (origin >= 0) {
        if (delta > static_cast<std::uint64_t>(ns_max - origin)) return std::nullopt;
        return origin + static_cast<Nanoseconds>(delta);
    }
    const auto magnitude = static_cast<std::uint64_t>(-(origin + 1)) + 1U;
    if (delta >= magnitude) {
        const auto positive = delta - magnitude;
        if (positive > static_cast<std::uint64_t>(ns_max)) return std::nullopt;
        return static_cast<Nanoseconds>(positive);
    }
    const auto negative = magnitude - delta;
    if (negative == static_cast<std::uint64_t>(ns_max) + 1U) return ns_min;
    return -static_cast<Nanoseconds>(negative);
}

void require_epoch(SessionToken epoch)
{
    if (!epoch.valid()) throw std::invalid_argument("session and generation must be nonzero");
}
} // namespace

std::optional<Nanoseconds> checked_add(Nanoseconds a, Nanoseconds b) noexcept
{
    if ((b > 0 && a > ns_max - b) || (b < 0 && a < ns_min - b)) return std::nullopt;
    return a + b;
}

std::optional<Nanoseconds> checked_sub(Nanoseconds a, Nanoseconds b) noexcept
{
    if ((b > 0 && a < ns_min + b) || (b < 0 && a > ns_max + b)) return std::nullopt;
    return a - b;
}

bool valid_successor(SessionToken current, SessionToken next) noexcept
{
    return next.valid() && (next.session != current.session || next.generation > current.generation);
}

CaptureToPresentation::CaptureToPresentation(SessionToken epoch, Nanoseconds total_delay, Nanoseconds path_offset)
    : epoch_(epoch), offset_(0)
{
    require_epoch(epoch);
    const auto offset = checked_add(total_delay, path_offset);
    if (total_delay < 0 || !offset || *offset < 0)
        throw std::invalid_argument("delay plus path offset must be representable and nonnegative");
    offset_ = *offset;
}

std::optional<Nanoseconds> CaptureToPresentation::map(SessionToken epoch, Nanoseconds capture) const noexcept
{
    if (epoch != epoch_) return std::nullopt;
    return checked_add(capture, offset_);
}

RationalTimeline::RationalTimeline(Nanoseconds origin, std::uint32_t rate_numerator, std::uint32_t rate_denominator)
    : origin_(origin), numerator_(rate_numerator), denominator_(rate_denominator)
{
    if (numerator_ == 0 || denominator_ == 0) throw std::invalid_argument("rate must be positive");
}

std::optional<Nanoseconds> RationalTimeline::at(std::uint64_t index) const noexcept
{
    const auto delta = mul_div(index, 1'000'000'000ULL * denominator_, numerator_);
    if (!delta) return std::nullopt;
    return add_unsigned(origin_, *delta);
}

BoundedTimelineQueue::BoundedTimelineQueue(SessionToken epoch, QueueLimits limits)
    : epoch_(epoch), limits_(limits)
{
    require_epoch(epoch);
    if (limits.max_count == 0 || limits.max_bytes == 0 || limits.max_future <= 0 || limits.max_span <= 0)
        throw std::invalid_argument("all queue limits must be positive");
}

PushStatus BoundedTimelineQueue::push(TimedMetadata item, Nanoseconds now)
{
    if (item.epoch != epoch_) return PushStatus::wrong_epoch;
    const auto end = checked_add(item.presentation_time, item.duration);
    if (item.duration <= 0 || item.payload_bytes == 0 || !end || !checked_add(item.capture_time, item.duration))
        return PushStatus::invalid;
    if (item.presentation_time < now) return PushStatus::late;
    const auto future = checked_sub(*end, now);
    if (!future || *future > limits_.max_future) return PushStatus::too_far_future;
    if (std::any_of(queue_.begin(), queue_.end(), [&](const auto& old) { return old.sequence == item.sequence; }))
        return PushStatus::duplicate;
    if (queue_.size() >= limits_.max_count || item.payload_bytes > limits_.max_bytes - bytes_)
        return PushStatus::full;
    auto first = item.presentation_time;
    auto last = *end;
    for (const auto& old : queue_) {
        first = std::min(first, old.presentation_time);
        // End-time representability was checked when old was inserted.
        last = std::max(last, *checked_add(old.presentation_time, old.duration));
    }
    const auto span = checked_sub(last, first);
    if (!span || *span > limits_.max_span) return PushStatus::full;
    const auto position = std::lower_bound(queue_.begin(), queue_.end(), item,
        [](const auto& a, const auto& b) {
            return a.presentation_time < b.presentation_time ||
                (a.presentation_time == b.presentation_time && a.sequence < b.sequence);
        });
    queue_.insert(position, item);
    bytes_ += item.payload_bytes;
    return PushStatus::accepted;
}

DueResult BoundedTimelineQueue::pop_due(Nanoseconds now, Nanoseconds allowed_lateness)
{
    if (allowed_lateness < 0) throw std::invalid_argument("allowed lateness must not be negative");
    DueResult result;
    if (queue_.empty() || queue_.front().presentation_time > now) return result;
    // Allocate before removing anything: an allocation failure must not lose
    // expired identities that the external payload owner still needs to free.
    result.discarded_late.reserve(queue_.size());
    while (!queue_.empty() && queue_.front().presentation_time <= now) {
        auto item = queue_.front();
        queue_.pop_front();
        bytes_ -= item.payload_bytes;
        const auto late = checked_sub(now, item.presentation_time);
        if (!late || *late > allowed_lateness) {
            result.discarded_late.push_back(item);
            continue;
        }
        result.item = item;
        break;
    }
    return result;
}

bool BoundedTimelineQueue::reset(SessionToken next) noexcept
{
    if (!valid_successor(epoch_, next)) return false;
    queue_.clear();
    bytes_ = 0;
    epoch_ = next;
    return true;
}

SampleRateEstimator::SampleRateEstimator(SessionToken epoch, RateEstimatorConfig config)
    : epoch_(epoch), config_(config)
{
    require_epoch(epoch);
    if (config.nominal_rate == 0 || !std::isfinite(config.max_correction_ppm) ||
        config.max_correction_ppm <= 0 || config.max_correction_ppm >= 1'000'000 ||
        config.min_window <= 0 || config.max_observation_gap <= 0)
        throw std::invalid_argument("invalid sample rate estimator configuration");
}

RateObservation SampleRateEstimator::observe(SessionToken epoch, std::uint64_t sample_position, Nanoseconds capture_time) noexcept
{
    if (epoch != epoch_) return {RateStatus::wrong_epoch, std::nullopt};
    const Point point{sample_position, capture_time};
    if (!last_) {
        anchor_ = last_ = point;
        return {RateStatus::priming, std::nullopt};
    }
    const auto gap = checked_sub(capture_time, last_->time);
    const auto window = checked_sub(capture_time, anchor_->time);
    if (!gap || *gap <= 0 || *gap > config_.max_observation_gap ||
        sample_position <= last_->sample || !window) {
        anchor_ = last_ = point;
        return {RateStatus::discontinuity, std::nullopt};
    }
    last_ = point;
    if (*window < config_.min_window) return {RateStatus::waiting, std::nullopt};
    const auto samples = sample_position - anchor_->sample;
    const double measured = static_cast<double>(samples) * 1e9 / static_cast<double>(*window);
    const double ppm = (measured / config_.nominal_rate - 1.0) * 1e6;
    const double bounded = std::clamp(ppm, -config_.max_correction_ppm, config_.max_correction_ppm);
    // A tiny tolerance only covers floating-point arithmetic at the threshold.
    const bool within = std::abs(ppm) <= config_.max_correction_ppm + 1e-6;
    const RateEstimate estimate{measured, ppm, bounded, 1.0 / (1.0 + bounded / 1e6), within};
    anchor_ = point;
    return {RateStatus::measured, estimate};
}

bool SampleRateEstimator::reset(SessionToken next) noexcept
{
    if (!valid_successor(epoch_, next)) return false;
    epoch_ = next;
    anchor_.reset();
    last_.reset();
    return true;
}

MicPrivacyGate::MicPrivacyGate(SessionToken epoch) : epoch_(epoch)
{
    require_epoch(epoch);
}

bool MicPrivacyGate::set_muted(bool muted, Nanoseconds control_time) noexcept
{
    if (last_control_ && control_time < *last_control_) return false;
    last_control_ = control_time;
    // Idempotent health/startup commands must not discard another delay-window
    // of voice. Only an actual mute-state edge establishes a new cutoff.
    if (muted == muted_) return true;
    muted_ = muted;
    cutoff_ = control_time;
    return true;
}

bool MicPrivacyGate::permits(SessionToken epoch, Nanoseconds capture_start, Nanoseconds duration) const noexcept
{
    return epoch == epoch_ && !muted_ && cutoff_ && capture_start >= *cutoff_ &&
        duration > 0 && checked_add(capture_start, duration).has_value();
}

bool MicPrivacyGate::reset(SessionToken next) noexcept
{
    if (!valid_successor(epoch_, next)) return false;
    epoch_ = next;
    muted_ = true;
    cutoff_.reset();
    last_control_.reset();
    return true;
}

} // namespace avsync
