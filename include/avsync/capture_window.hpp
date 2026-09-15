// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace avsync {

struct CaptureWindowPolicy {
    std::int64_t maximum_future_ns = 100'000'000;
    std::int64_t maximum_age_ns = 100'000'000;
};

enum class CaptureWindowStatus { accepted, invalid, too_far_future, too_old };

// A loopback endpoint can return a packet before its timestamped presentation
// instant. Eligibility is bounded on both sides; the original timestamp is
// never changed. Nonnegative signed times make each ordered subtraction safe,
// including near INT64_MAX. Exact window boundaries are accepted.
[[nodiscard]] constexpr CaptureWindowStatus capture_window_status(
    std::int64_t capture_ns, std::int64_t now_ns, CaptureWindowPolicy policy = {}) noexcept
{
    if (capture_ns < 0 || now_ns < 0 || policy.maximum_future_ns < 0 || policy.maximum_age_ns < 0)
        return CaptureWindowStatus::invalid;
    if (capture_ns > now_ns)
        return capture_ns - now_ns <= policy.maximum_future_ns ?
            CaptureWindowStatus::accepted : CaptureWindowStatus::too_far_future;
    return now_ns - capture_ns <= policy.maximum_age_ns ?
        CaptureWindowStatus::accepted : CaptureWindowStatus::too_old;
}

} // namespace avsync
