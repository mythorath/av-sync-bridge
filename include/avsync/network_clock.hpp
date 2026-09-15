// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/timing.hpp"
#include <gst/gst.h>
#include <cstdint>
#include <optional>

namespace avsync::net {

struct Calibration {
    Nanoseconds internal_reference = 0;
    Nanoseconds external_reference = 0;
    std::uint64_t rate_numerator = 1;
    std::uint64_t rate_denominator = 1;
};

// Maps a historical local monotonic timestamp, not a current arrival timestamp.
// Uses the same floor-scaled affine relation as GstClock, without monotonic-now
// clamping. Negative or overflowing results are rejected in this clock domain.
[[nodiscard]] std::optional<Nanoseconds> apply_calibration(const Calibration&, Nanoseconds local_ns) noexcept;
[[nodiscard]] std::optional<Nanoseconds> qpc_100ns_to_ns(std::uint64_t qpc_100ns) noexcept;

// The NetClientClock wrapper's internal time is already remote-adjusted. This
// reads its underlying "internal-clock" calibration, not the wrapper's identity.
// The caller owns the clock; no sockets, providers, or clocks are created here.
[[nodiscard]] std::optional<Calibration> client_calibration(GstClock* client) noexcept;
[[nodiscard]] std::optional<Nanoseconds> map_qpc_100ns(GstClock* client, std::uint64_t qpc_100ns) noexcept;

struct DomainCheck {
    bool valid = false;
    Nanoseconds bracket_width_ns = 0;
    Nanoseconds outside_bracket_ns = 0;
};

// Pass an unadjusted monotonic GstSystemClock (or NetClientClock's underlying
// internal-clock), NOT the remote-adjusted NetClientClock wrapper. Compares to
// Windows QPC / Linux CLOCK_MONOTONIC using bounded before/after sampling.
[[nodiscard]] DomainCheck verify_local_monotonic_domain(GstClock* local_clock,
                                                       Nanoseconds tolerance_ns = 1'000'000) noexcept;

struct ClockHealthPolicy {
    std::uint64_t minimum_observations = 4;
    Nanoseconds maximum_observation_age_ns = 2'000'000'000;
    Nanoseconds maximum_rtt_ns = 5'000'000;
    Nanoseconds maximum_discontinuity_ns = 2'000'000;
    double maximum_rate_error_ppm = 2'000.0;
};

struct ClockStatistics {
    Nanoseconds local_receive_ns = 0;
    Nanoseconds rtt_ns = 0;
    Nanoseconds discontinuity_ns = 0;
    bool algorithm_synchronized = false;
};

struct ClockHealth {
    bool usable = false;
    bool synchronized = false;
    const char* reason = "no statistics";
    std::uint64_t observations = 0;
    std::optional<Nanoseconds> observation_age_ns;
    std::optional<Nanoseconds> rtt_ns;
    std::optional<double> rate_error_ppm;
};

// Single-owner state. Drain the client's GstBus regularly and pass ELEMENT
// messages here. No bus/property ownership is changed. Policy thresholds are
// operational gates, NOT statistical or guaranteed bounds on clock offset.
class ClockHealthMonitor {
public:
    explicit ClockHealthMonitor(ClockHealthPolicy policy = {});
    [[nodiscard]] bool observe(GstMessage* message) noexcept;
    [[nodiscard]] ClockHealth health(GstClock* client) const noexcept;
    // Dependency-injected evaluation for deterministic tests and diagnostics.
    [[nodiscard]] ClockHealth health(bool synchronized, const std::optional<Calibration>& calibration,
                                     Nanoseconds local_now_ns) const noexcept;
    void reset() noexcept;
private:
    ClockHealthPolicy policy_;
    std::optional<ClockStatistics> last_;
    std::uint64_t observations_ = 0;
};

} // namespace avsync::net
