// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/timing.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>

namespace avsync {

// A nominal conversion position, not a capture timestamp or rounded RTP PTS.
// Value is whole + remainder / denominator; denominator remains the source rate.
struct ExactWirePosition {
    std::uint64_t whole = 0;
    std::uint32_t remainder = 0;
    std::uint32_t denominator = 1;
    friend bool operator==(const ExactWirePosition&, const ExactWirePosition&) = default;
};

[[nodiscard]] inline std::optional<ExactWirePosition> nominal_wire_position(
    std::uint64_t device_position, std::uint64_t device_origin,
    std::uint64_t wire_origin, std::uint32_t source_rate) noexcept
{
    if (source_rate == 0 || device_position < device_origin) return std::nullopt;
    constexpr std::uint64_t wire_rate = 48'000;
    constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
    const auto delta = device_position - device_origin;
    const auto quotient = delta / source_rate;
    // remainder < UINT32_MAX, so this multiplication is representable.
    const auto numerator = (delta % source_rate) * wire_rate;
    if (quotient > maximum / wire_rate) return std::nullopt;
    auto whole = quotient * wire_rate;
    const auto carry = numerator / source_rate;
    if (carry > maximum - whole) return std::nullopt;
    whole += carry;
    if (wire_origin > maximum - whole) return std::nullopt;
    return ExactWirePosition{whole + wire_origin,
        static_cast<std::uint32_t>(numerator % source_rate), source_rate};
}

struct AudioCaptureAnchor {
    SessionToken epoch;
    std::uint64_t sequence = 0;
    std::uint64_t device_frame_position = 0;
    Nanoseconds capture_ns = 0; // Original capture time already mapped to shared clock.
    std::uint32_t nominal_source_rate = 48'000;
    bool discontinuity = false;
    friend bool operator==(const AudioCaptureAnchor&, const AudioCaptureAnchor&) = default;
};

struct AudioAnchorConfig {
    RateEstimatorConfig rate; // Uses ORIGINAL device positions and capture times.
    std::uint64_t device_frame_origin = 0;
    std::uint64_t wire_frame_origin = 0;
    Nanoseconds max_anchor_age_ns = 250'000'000;
    Nanoseconds max_future_ns = 1'000'000;
};

enum class AudioAnchorStatus {
    priming, waiting, measured, wrong_epoch, invalid, stale, future,
    discontinuity, overflow, requires_reset
};

struct AudioAnchorObservation {
    AudioAnchorStatus status;
    std::optional<ExactWirePosition> wire_position;
    std::optional<RateEstimate> estimate;
};

struct AudioAnchorDiagnostics {
    std::uint64_t accepted = 0;
    std::uint64_t rejected = 0;
    std::uint64_t measurements = 0;
    std::uint64_t wrong_epoch = 0;
    std::uint64_t stale = 0;
    std::uint64_t future = 0;
    std::uint64_t invalid = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t overflows = 0;
    std::uint64_t resets = 0;
};

// One source and one explicit nominal-conversion segment. O(1) metadata only;
// no allocation, PCM storage, arrival-time rebasing, or adaptive resampling.
class AudioAnchorTracker {
public:
    explicit AudioAnchorTracker(SessionToken epoch, AudioAnchorConfig config = {})
        : epoch_(epoch), config_(validated(config)), estimator_(epoch, config_.rate) {}

    [[nodiscard]] AudioAnchorObservation observe(
        const AudioCaptureAnchor& anchor, Nanoseconds shared_now) noexcept
    {
        // Stray generations do not poison or re-prime the current stream.
        if (anchor.epoch != epoch_) return reject(AudioAnchorStatus::wrong_epoch, false);
        if (faulted_) return reject(AudioAnchorStatus::requires_reset, false);
        if (anchor.nominal_source_rate == 0 || anchor.capture_ns < 0 || shared_now < 0)
            return reject(AudioAnchorStatus::invalid);
        if (anchor.discontinuity || anchor.nominal_source_rate != config_.rate.nominal_rate)
            return reject(AudioAnchorStatus::discontinuity);
        const auto age = checked_sub(shared_now, anchor.capture_ns);
        if (!age) return reject(AudioAnchorStatus::overflow);
        if (*age > config_.max_anchor_age_ns) return reject(AudioAnchorStatus::stale);
        if (*age < -config_.max_future_ns) return reject(AudioAnchorStatus::future);
        if (last_) {
            const auto gap = checked_sub(anchor.capture_ns, last_->capture_ns);
            if (!gap) return reject(AudioAnchorStatus::overflow);
            if (last_->sequence == std::numeric_limits<std::uint64_t>::max() ||
                anchor.sequence != last_->sequence + 1 ||
                anchor.device_frame_position <= last_->device_frame_position ||
                *gap <= 0 || *gap > config_.rate.max_observation_gap)
                return reject(AudioAnchorStatus::discontinuity);
        }
        const auto wire = nominal_wire_position(anchor.device_frame_position,
            config_.device_frame_origin, config_.wire_frame_origin, config_.rate.nominal_rate);
        if (!wire) return reject(anchor.device_frame_position < config_.device_frame_origin
            ? AudioAnchorStatus::invalid : AudioAnchorStatus::overflow);
        const auto rate = estimator_.observe(epoch_, anchor.device_frame_position, anchor.capture_ns);
        if (rate.status == RateStatus::discontinuity || rate.status == RateStatus::wrong_epoch)
            return reject(AudioAnchorStatus::discontinuity);
        last_ = anchor;
        last_wire_ = wire;
        increment(diagnostics_.accepted);
        if (rate.estimate) {
            estimate_ = rate.estimate;
            increment(diagnostics_.measurements);
        }
        const auto status = rate.status == RateStatus::priming ? AudioAnchorStatus::priming :
            rate.status == RateStatus::measured ? AudioAnchorStatus::measured : AudioAnchorStatus::waiting;
        return {status, wire, rate.estimate};
    }

    // Expiry is checked even when no packets arrive. This is read-only: a
    // transport/controller must abandon an expired generation before reuse.
    [[nodiscard]] bool fresh(Nanoseconds shared_now) const noexcept
    {
        if (faulted_ || !last_ || shared_now < 0) return false;
        const auto age = checked_sub(shared_now, last_->capture_ns);
        return age && *age >= -config_.max_future_ns && *age <= config_.max_anchor_age_ns;
    }

    // Never expose a clamped, stale, or invalid estimate as an approved command.
    [[nodiscard]] std::optional<RateEstimate> current_estimate(Nanoseconds shared_now) const noexcept
    {
        if (!fresh(shared_now) || !estimate_ || !estimate_->within_correction_limit)
            return std::nullopt;
        return estimate_;
    }

    // Explicit new conversion origins are mandatory on reset. Invalid config
    // throws before any state changes; a wrong/reused generation leaves it intact.
    [[nodiscard]] bool reset(SessionToken next, AudioAnchorConfig config)
    {
        if (!valid_successor(epoch_, next)) return false;
        auto next_config = validated(config);
        SampleRateEstimator next_estimator(next, next_config.rate);
        epoch_ = next;
        config_ = next_config;
        estimator_ = next_estimator;
        last_.reset();
        last_wire_.reset();
        estimate_.reset();
        faulted_ = false;
        increment(diagnostics_.resets);
        return true;
    }

    // Diagnostic snapshots only; these do not assert current freshness.
    [[nodiscard]] const std::optional<AudioCaptureAnchor>& latest() const noexcept { return last_; }
    [[nodiscard]] const std::optional<ExactWirePosition>& latest_wire_position() const noexcept { return last_wire_; }
    [[nodiscard]] const AudioAnchorDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] SessionToken epoch() const noexcept { return epoch_; }
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }

private:
    [[nodiscard]] static AudioAnchorConfig validated(AudioAnchorConfig config)
    {
        if (config.max_anchor_age_ns <= 0 || config.max_future_ns < 0)
            throw std::invalid_argument("invalid audio anchor age bounds");
        return config;
    }

    static void increment(std::uint64_t& count) noexcept
    {
        if (count != std::numeric_limits<std::uint64_t>::max()) ++count;
    }

    [[nodiscard]] AudioAnchorObservation reject(AudioAnchorStatus status, bool poison = true) noexcept
    {
        increment(diagnostics_.rejected);
        if (poison) faulted_ = true;
        switch (status) {
        case AudioAnchorStatus::wrong_epoch: increment(diagnostics_.wrong_epoch); break;
        case AudioAnchorStatus::stale: increment(diagnostics_.stale); break;
        case AudioAnchorStatus::future: increment(diagnostics_.future); break;
        case AudioAnchorStatus::invalid: increment(diagnostics_.invalid); break;
        case AudioAnchorStatus::discontinuity: increment(diagnostics_.discontinuities); break;
        case AudioAnchorStatus::overflow: increment(diagnostics_.overflows); break;
        default: break;
        }
        return {status, std::nullopt, std::nullopt};
    }

    SessionToken epoch_;
    AudioAnchorConfig config_;
    SampleRateEstimator estimator_;
    std::optional<AudioCaptureAnchor> last_;
    std::optional<ExactWirePosition> last_wire_;
    std::optional<RateEstimate> estimate_;
    AudioAnchorDiagnostics diagnostics_;
    bool faulted_ = false;
};

} // namespace avsync
