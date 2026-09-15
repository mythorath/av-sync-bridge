// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/network_clock.hpp"
#include <gst/net/gstnetclientclock.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__linux__)
#include <time.h>
#endif

namespace avsync::net {
namespace {
constexpr auto maximum = std::numeric_limits<Nanoseconds>::max();

std::optional<Nanoseconds> signed_time(GstClockTime time) noexcept
{
    if (!GST_CLOCK_TIME_IS_VALID(time) || time > static_cast<GstClockTime>(maximum)) return std::nullopt;
    return static_cast<Nanoseconds>(time);
}

bool valid_calibration(const Calibration& value) noexcept
{
    return value.internal_reference >= 0 && value.external_reference >= 0 &&
        value.rate_numerator != 0 && value.rate_denominator != 0;
}

bool is_monotonic_system_clock(GstClock* clock) noexcept
{
    if (!clock || !GST_IS_SYSTEM_CLOCK(clock)) return false;
    GstClockType type = GST_CLOCK_TYPE_REALTIME;
    g_object_get(clock, "clock-type", &type, nullptr);
    return type == GST_CLOCK_TYPE_MONOTONIC;
}

std::optional<Nanoseconds> operating_system_monotonic() noexcept
{
#ifdef _WIN32
    LARGE_INTEGER now{}, frequency{};
    if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0 ||
        !QueryPerformanceCounter(&now) || now.QuadPart < 0) return std::nullopt;
    return signed_time(gst_util_uint64_scale(static_cast<guint64>(now.QuadPart), GST_SECOND,
                                            static_cast<guint64>(frequency.QuadPart)));
#elif defined(__linux__)
    timespec now{};
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0 || now.tv_sec < 0 || now.tv_nsec < 0 ||
        now.tv_nsec >= 1'000'000'000 || static_cast<std::uint64_t>(now.tv_sec) >
        static_cast<std::uint64_t>(maximum) / 1'000'000'000) return std::nullopt;
    return checked_add(static_cast<Nanoseconds>(now.tv_sec) * 1'000'000'000,
                       static_cast<Nanoseconds>(now.tv_nsec));
#else
    return std::nullopt;
#endif
}
} // namespace

std::optional<Nanoseconds> apply_calibration(const Calibration& calibration, Nanoseconds local_ns) noexcept
{
    if (!valid_calibration(calibration) || local_ns < 0) return std::nullopt;
    const bool forward = local_ns >= calibration.internal_reference;
    const auto delta = static_cast<std::uint64_t>(forward ? local_ns - calibration.internal_reference :
                                                calibration.internal_reference - local_ns);
    const auto scaled = signed_time(gst_util_uint64_scale(delta, calibration.rate_numerator,
                                                         calibration.rate_denominator));
    if (!scaled) return std::nullopt;
    const auto mapped = forward ? checked_add(calibration.external_reference, *scaled) :
                                  checked_sub(calibration.external_reference, *scaled);
    if (!mapped || *mapped < 0) return std::nullopt;
    return mapped;
}

std::optional<Nanoseconds> qpc_100ns_to_ns(std::uint64_t qpc_100ns) noexcept
{
    if (qpc_100ns > static_cast<std::uint64_t>(maximum) / 100) return std::nullopt;
    return static_cast<Nanoseconds>(qpc_100ns * 100);
}

std::optional<Calibration> client_calibration(GstClock* client) noexcept
{
    if (!client || !GST_IS_NET_CLIENT_CLOCK(client) || !gst_clock_is_synced(client)) return std::nullopt;
    GstClockTime outer_internal, outer_external, outer_num, outer_den;
    gst_clock_get_calibration(client, &outer_internal, &outer_external, &outer_num, &outer_den);
    // This helper's contract does not support a second, user-applied wrapper
    // transform. Refuse it rather than silently dropping or applying it twice.
    if (outer_internal != outer_external || outer_num == 0 || outer_num != outer_den) return std::nullopt;
    GstClock* internal = nullptr;
    g_object_get(client, "internal-clock", &internal, nullptr);
    if (!internal) return std::nullopt;
    std::optional<Calibration> result;
    if (is_monotonic_system_clock(internal) && gst_clock_is_synced(internal)) {
        GstClockTime local, remote, numerator, denominator;
        gst_clock_get_calibration(internal, &local, &remote, &numerator, &denominator);
        const auto local_signed = signed_time(local), remote_signed = signed_time(remote);
        if (local_signed && remote_signed && numerator != 0 && denominator != 0)
            result = Calibration{*local_signed, *remote_signed, numerator, denominator};
    }
    gst_object_unref(internal);
    return result;
}

std::optional<Nanoseconds> map_qpc_100ns(GstClock* client, std::uint64_t qpc_100ns) noexcept
{
    const auto local = qpc_100ns_to_ns(qpc_100ns);
    const auto calibration = client_calibration(client);
    if (!local || !calibration) return std::nullopt;
    return apply_calibration(*calibration, *local);
}

CaptureClockMapper::CaptureClockMapper(std::uint64_t maximum_revision)
    : maximum_revision_(maximum_revision)
{
    if (maximum_revision == 0)
        throw std::invalid_argument("capture clock revision budget must be nonzero");
}

std::optional<MappedCapture> CaptureClockMapper::map(
    GstClock* client, std::uint64_t qpc_100ns) noexcept
{
    // One calibration query supplies both the transform and returned
    // provenance. Later clock recalibration cannot mutate this local value.
    const auto snapshot = client_calibration(client);
    if (!snapshot) return std::nullopt;
    return map(*snapshot, qpc_100ns);
}

std::optional<MappedCapture> CaptureClockMapper::map(
    const Calibration& calibration, std::uint64_t qpc_100ns) noexcept
{
    const Calibration snapshot = calibration;
    const auto local = qpc_100ns_to_ns(qpc_100ns);
    if (!local) return std::nullopt;
    const auto capture = apply_calibration(snapshot, *local);
    if (!capture) return std::nullopt;

    const bool changed = !previous_ || *previous_ != snapshot;
    if (changed && revision_ == maximum_revision_) return std::nullopt;
    // Complete every validation before advancing revision/provenance. With
    // the exhaustion check, increment cannot wrap even at the default budget.
    const auto next_revision = revision_ + (changed ? 1U : 0U);
    const MappedCapture result{*local, *capture, snapshot, next_revision};
    previous_ = snapshot;
    revision_ = next_revision;
    return result;
}

DomainCheck verify_local_monotonic_domain(GstClock* local_clock, Nanoseconds tolerance_ns) noexcept
{
    DomainCheck best;
    best.bracket_width_ns = maximum;
    if (tolerance_ns < 0 || !is_monotonic_system_clock(local_clock)) return best;
    for (unsigned attempt = 0; attempt < 8; ++attempt) {
        const auto before = operating_system_monotonic();
        const auto internal = signed_time(gst_clock_get_internal_time(local_clock));
        const auto after = operating_system_monotonic();
        if (!before || !internal || !after || *after < *before) continue;
        const auto width = *after - *before;
        Nanoseconds outside = 0;
        if (*internal < *before) outside = *internal - *before;
        if (*internal > *after) outside = *internal - *after;
        if (width < best.bracket_width_ns) {
            best.bracket_width_ns = width;
            best.outside_bracket_ns = outside;
            best.valid = width <= tolerance_ns && outside <= tolerance_ns && outside >= -tolerance_ns;
        }
    }
    return best;
}

ClockHealthMonitor::ClockHealthMonitor(ClockHealthPolicy policy) : policy_(policy)
{
    if (policy.minimum_observations == 0 || policy.maximum_observation_age_ns <= 0 ||
        policy.maximum_rtt_ns <= 0 || policy.maximum_discontinuity_ns < 0 ||
        !std::isfinite(policy.maximum_rate_error_ppm) || policy.maximum_rate_error_ppm <= 0)
        throw std::invalid_argument("invalid network clock health policy");
}

bool ClockHealthMonitor::observe(GstMessage* message) noexcept
{
    if (!message || GST_MESSAGE_TYPE(message) != GST_MESSAGE_ELEMENT) return false;
    const GstStructure* data = gst_message_get_structure(message);
    if (!data || !gst_structure_has_name(data, "gst-netclock-statistics")) return false;
    guint64 receive = 0, rtt = 0;
    gint64 discontinuity = 0;
    gboolean algorithm_synced = FALSE;
    if (!gst_structure_get_uint64(data, "request-receive", &receive) ||
        !gst_structure_get_uint64(data, "rtt", &rtt) ||
        !gst_structure_get_int64(data, "discontinuity", &discontinuity) ||
        !gst_structure_get_boolean(data, "synchronised", &algorithm_synced)) return false;
    const auto receive_ns = signed_time(receive), rtt_ns = signed_time(rtt);
    if (!receive_ns || !rtt_ns || (last_ && *receive_ns <= last_->local_receive_ns)) return false;
    last_ = ClockStatistics{*receive_ns, *rtt_ns, discontinuity, algorithm_synced != FALSE};
    if (observations_ != std::numeric_limits<std::uint64_t>::max()) ++observations_;
    return true;
}

ClockHealth ClockHealthMonitor::health(GstClock* client) const noexcept
{
    const auto now = signed_time(gst_util_get_timestamp());
    if (!now) return {};
    return health(client && gst_clock_is_synced(client), client_calibration(client), *now);
}

ClockHealth ClockHealthMonitor::health(bool synchronized, const std::optional<Calibration>& calibration,
                                      Nanoseconds local_now_ns) const noexcept
{
    ClockHealth result;
    result.synchronized = synchronized;
    result.observations = observations_;
    if (!synchronized || !calibration || !valid_calibration(*calibration)) {
        result.reason = "clock is not synchronized or calibration is invalid";
        return result;
    }
    result.rate_error_ppm = (static_cast<double>(calibration->rate_numerator) /
                             static_cast<double>(calibration->rate_denominator) - 1.0) * 1e6;
    if (!last_) return result;
    result.rtt_ns = last_->rtt_ns;
    result.observation_age_ns = checked_sub(local_now_ns, last_->local_receive_ns);
    if (observations_ < policy_.minimum_observations) {
        result.reason = "insufficient clock observations";
    } else if (!result.observation_age_ns || *result.observation_age_ns < 0 ||
               *result.observation_age_ns > policy_.maximum_observation_age_ns) {
        result.reason = "clock observations are stale or from another local time domain";
    } else if (last_->rtt_ns > policy_.maximum_rtt_ns) {
        result.reason = "clock round trip exceeds policy";
    } else if (std::abs(*result.rate_error_ppm) > policy_.maximum_rate_error_ppm) {
        result.reason = "clock calibration rate exceeds policy";
    } else if (last_->discontinuity_ns > policy_.maximum_discontinuity_ns ||
               last_->discontinuity_ns < -policy_.maximum_discontinuity_ns) {
        result.reason = "clock calibration discontinuity exceeds policy";
    } else {
        result.usable = true;
        result.reason = "operational clock gates satisfied; error bound remains unproven";
    }
    return result;
}

void ClockHealthMonitor::reset() noexcept
{
    last_.reset();
    observations_ = 0;
}
} // namespace avsync::net
