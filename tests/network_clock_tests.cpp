// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/network_clock.hpp"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
using namespace avsync;
using namespace avsync::net;
constexpr auto maximum = std::numeric_limits<Nanoseconds>::max();

GstMessage* statistics(guint64 received, guint64 rtt = 100'000,
                       gint64 discontinuity = 0, gboolean synchronized = TRUE)
{
    return gst_message_new_element(nullptr, gst_structure_new("gst-netclock-statistics",
        "request-receive", G_TYPE_UINT64, received,
        "rtt", G_TYPE_UINT64, rtt,
        "discontinuity", G_TYPE_INT64, discontinuity,
        "synchronised", G_TYPE_BOOLEAN, synchronized, nullptr));
}

bool observe(ClockHealthMonitor& monitor, guint64 received, guint64 rtt = 100'000,
             gint64 discontinuity = 0, gboolean synchronized = TRUE)
{
    auto* message = statistics(received, rtt, discontinuity, synchronized);
    const bool accepted = monitor.observe(message);
    gst_message_unref(message);
    return accepted;
}

void test_mapping()
{
    const Calibration identity{0, 0, 1, 1};
    CHECK(apply_calibration(identity, 0) == 0);
    CHECK(apply_calibration(identity, maximum) == maximum);
    CHECK(!apply_calibration(identity, -1));
    CHECK(!apply_calibration(Calibration{-1, 0, 1, 1}, 0));
    CHECK(!apply_calibration(Calibration{0, -1, 1, 1}, 0));
    CHECK(!apply_calibration(Calibration{0, 0, 0, 1}, 0));
    CHECK(!apply_calibration(Calibration{0, 0, 1, 0}, 0));
    CHECK(!apply_calibration(Calibration{0, maximum, 1, 1}, 1));
    CHECK(!apply_calibration(Calibration{1, 0, 1, 1}, 0));
    CHECK(!apply_calibration(Calibration{0, 0, std::numeric_limits<std::uint64_t>::max(), 1}, maximum));
    CHECK(apply_calibration(Calibration{100, 500, 3, 2}, 103) == 504);
    CHECK(apply_calibration(Calibration{100, 500, 3, 2}, 97) == 496);
    CHECK(apply_calibration(Calibration{100, 500, 3, 2}, 100) == 500);
    // The mapped event is the historical capture time, not the time of this call.
    const Calibration historical{10'000'000'000, 50'000'000'000, 1, 1};
    CHECK(apply_calibration(historical, 9'000'000'000) == 49'000'000'000);
    CHECK(apply_calibration(historical, 9'000'000'000) == 49'000'000'000);
    for (const std::uint64_t numerator : {999'500ULL, 999'900ULL, 1'000'000ULL, 1'000'100ULL, 1'000'500ULL}) {
        const Calibration value{20'000'000'000, 80'000'000'000, numerator, 1'000'000};
        for (Nanoseconds delta = -10'000'000'000; delta <= 10'000'000'000; delta += 19'876'543) {
            const auto local = value.internal_reference + delta;
            const auto mapped = apply_calibration(value, local);
            CHECK(mapped);
            const auto reference = gst_clock_adjust_with_calibration(nullptr, static_cast<GstClockTime>(local),
                static_cast<GstClockTime>(value.internal_reference), static_cast<GstClockTime>(value.external_reference),
                value.rate_numerator, value.rate_denominator);
            CHECK(*mapped == static_cast<Nanoseconds>(reference));
        }
    }
    CHECK(qpc_100ns_to_ns(0) == 0);
    CHECK(qpc_100ns_to_ns(1234567) == 123456700);
    const auto limit = static_cast<std::uint64_t>(maximum) / 100;
    CHECK(qpc_100ns_to_ns(limit) == static_cast<Nanoseconds>(limit * 100));
    CHECK(!qpc_100ns_to_ns(limit + 1));
    CHECK(!qpc_100ns_to_ns(std::numeric_limits<std::uint64_t>::max()));
    CHECK(!client_calibration(nullptr));
    CHECK(!map_qpc_100ns(nullptr, 0));
}

void test_health()
{
    const Calibration identity{0, 0, 1, 1};
    ClockHealthMonitor monitor;
    CHECK(!monitor.health(true, identity, 1'000'000'000).usable);
    CHECK(!monitor.observe(nullptr));
    auto* unrelated = gst_message_new_element(nullptr, gst_structure_new_empty("different-message"));
    CHECK(!monitor.observe(unrelated));
    gst_message_unref(unrelated);
    auto* malformed = gst_message_new_element(nullptr, gst_structure_new_empty("gst-netclock-statistics"));
    CHECK(!monitor.observe(malformed));
    gst_message_unref(malformed);
    auto* eos = gst_message_new_eos(nullptr);
    CHECK(!monitor.observe(eos));
    gst_message_unref(eos);
    for (guint64 i = 1; i <= 4; ++i) {
        CHECK(observe(monitor, i * 100'000'000));
        const auto health = monitor.health(true, identity, static_cast<Nanoseconds>(i * 100'000'000 + 1));
        CHECK(health.observations == i);
        CHECK(health.usable == (i == 4));
    }
    const auto healthy = monitor.health(true, identity, 500'000'000);
    CHECK(healthy.usable);
    CHECK(healthy.observation_age_ns == 100'000'000);
    CHECK(healthy.rtt_ns == 100'000);
    CHECK(healthy.rate_error_ppm == 0);
    CHECK(!observe(monitor, 400'000'000));
    CHECK(!observe(monitor, 399'999'999));
    CHECK(!observe(monitor, GST_CLOCK_TIME_NONE));
    CHECK(!observe(monitor, 500'000'000, GST_CLOCK_TIME_NONE));
    CHECK(monitor.health(true, identity, 500'000'000).observations == 4);
    CHECK(!monitor.health(false, identity, 500'000'000).usable);
    CHECK(!monitor.health(true, std::nullopt, 500'000'000).usable);
    CHECK(!monitor.health(true, Calibration{0, 0, 1, 0}, 500'000'000).usable);
    CHECK(!monitor.health(true, identity, 399'999'999).usable);
    CHECK(monitor.health(true, identity, 2'400'000'000).usable);
    CHECK(!monitor.health(true, identity, 2'400'000'001).usable);
    CHECK(!monitor.health(true, identity, std::numeric_limits<Nanoseconds>::min()).usable);
    for (const std::uint64_t rate : {999'500ULL, 999'900ULL, 1'000'100ULL, 1'000'500ULL})
        CHECK(monitor.health(true, Calibration{0, 0, rate, 1'000'000}, 500'000'000).usable);
    CHECK(!monitor.health(true, Calibration{0, 0, 1'003'000, 1'000'000}, 500'000'000).usable);
    CHECK(!monitor.health(true, Calibration{0, 0, 997'000, 1'000'000}, 500'000'000).usable);
    CHECK(observe(monitor, 500'000'000, 5'000'001));
    CHECK(!monitor.health(true, identity, 500'000'001).usable);
    CHECK(observe(monitor, 600'000'000, 100'000, 2'000'001));
    CHECK(!monitor.health(true, identity, 600'000'001).usable);
    CHECK(observe(monitor, 700'000'000, 100'000, -2'000'001));
    CHECK(!monitor.health(true, identity, 700'000'001).usable);
    CHECK(observe(monitor, 800'000'000, 100'000, std::numeric_limits<gint64>::min()));
    CHECK(!monitor.health(true, identity, 800'000'001).usable);
    CHECK(observe(monitor, 900'000'000, 5'000'000, -2'000'000));
    CHECK(monitor.health(true, identity, 900'000'001).usable);
    // The source's per-observation "synchronised" field is diagnostic, not a
    // substitute for the public clock sync status and fresh-calibration gates.
    CHECK(observe(monitor, 1'000'000'000, 100'000, 0, FALSE));
    CHECK(monitor.health(true, identity, 1'000'000'001).usable);
    monitor.reset();
    CHECK(!monitor.health(true, identity, 1'000'000'001).usable);
    CHECK(monitor.health(true, identity, 1'000'000'001).observations == 0);
    CHECK(!monitor.health(static_cast<GstClock*>(nullptr)).usable);
    for (unsigned kind = 0; kind < 6; ++kind) {
        ClockHealthPolicy policy;
        if (kind == 0) policy.minimum_observations = 0;
        if (kind == 1) policy.maximum_observation_age_ns = 0;
        if (kind == 2) policy.maximum_rtt_ns = 0;
        if (kind == 3) policy.maximum_discontinuity_ns = -1;
        if (kind == 4) policy.maximum_rate_error_ppm = std::numeric_limits<double>::quiet_NaN();
        if (kind == 5) policy.maximum_rate_error_ppm = 0;
        bool threw = false;
        try { ClockHealthMonitor invalid(policy); } catch (const std::invalid_argument&) { threw = true; }
        CHECK(threw);
    }
}

void test_capture_mapper()
{
    const Calibration first{10'000'000'000, 50'000'000'000, 1, 1};
    Calibration changed = first;
    CaptureClockMapper mapper;
    // Rejected input cannot consume revision 1 or retain invalid provenance.
    CHECK(!mapper.map(nullptr, 0));
    CHECK(!mapper.map(Calibration{0, 0, 1, 0}, 0));
    CHECK(!mapper.map(first, std::numeric_limits<std::uint64_t>::max()));
    const auto a = mapper.map(first, 90'000'000);
    CHECK(a);
    CHECK(a->local_ns == 9'000'000'000);
    CHECK(a->capture_ns == 49'000'000'000);
    CHECK(a->calibration == first);
    CHECK(a->revision == 1);
    // Historical time need not increase in the mapper: the anchor tracker
    // owns chronology. There is no monotonic-now clamping or arrival fallback.
    const auto older = mapper.map(first, 80'000'000);
    CHECK(older && older->capture_ns == 48'000'000'000 && older->revision == 1);
    CHECK(mapper.map(first, 90'000'000) == a);
    CHECK(!mapper.map(nullptr, 90'000'000));

    changed.external_reference += 125;
    const auto b = mapper.map(changed, 90'000'000);
    CHECK(b && b->capture_ns == a->capture_ns + 125 && b->revision == 2);
    CHECK(b->calibration == changed);
    CHECK(apply_calibration(b->calibration, b->local_ns) == b->capture_ns);
    // A caller's later calibration edit cannot alter an already returned
    // record or accidentally supply a separately queried diagnostic snapshot.
    changed.external_reference += 456;
    CHECK(a->calibration == first && a->revision == 1 && a->capture_ns == 49'000'000'000);
    CHECK(b->calibration.external_reference == first.external_reference + 125);
    CHECK(b->capture_ns == apply_calibration(b->calibration, b->local_ns));
    const auto c = mapper.map(changed, 90'000'000);
    CHECK(c && c->revision == 3 && c->capture_ns == b->capture_ns + 456);
    CHECK(mapper.map(first, 90'000'000)->revision == 4); // Reversion is a new revision.

    // Every coefficient participates in identity, even when an equivalent
    // transform maps this particular QPC value to the same result.
    const Calibration identity{0, 0, 1, 1};
    CaptureClockMapper coefficients;
    CHECK(coefficients.map(identity, 100)->revision == 1);
    for (const Calibration value : {Calibration{100, 100, 1, 1},
                                    Calibration{100, 100, 2, 2},
                                    Calibration{100, 100, 3, 2},
                                    Calibration{100, 100, 3, 4}}) {
        const auto record = coefficients.map(value, 100);
        CHECK(record);
        CHECK(record->calibration == value);
        CHECK(record->capture_ns == apply_calibration(value, 10'000));
        CHECK(coefficients.map(value, 100) == record);
    }
    CHECK(coefficients.map(Calibration{100, 100, 3, 4}, 100)->revision == 5);

    // Failures after a successful result must preserve both coefficient
    // identity and revision: next successful change advances by exactly one.
    CaptureClockMapper transactional;
    CHECK(transactional.map(identity, 0)->revision == 1);
    const auto qpc_limit = static_cast<std::uint64_t>(maximum) / 100;
    const auto boundary = transactional.map(identity, qpc_limit);
    CHECK(boundary && boundary->capture_ns == static_cast<Nanoseconds>(qpc_limit * 100));
    CHECK(boundary->revision == 1);
    CHECK(!transactional.map(Calibration{0, 1, 1, 1}, qpc_limit + 1));
    CHECK(!transactional.map(Calibration{0, maximum, 1, 1}, 1));
    CHECK(!transactional.map(Calibration{1, 0, 1, 1}, 0));
    CHECK(!transactional.map(Calibration{0, 0, std::numeric_limits<std::uint64_t>::max(), 1}, 1));
    CHECK(!transactional.map(Calibration{-1, 0, 1, 1}, 0));
    CHECK(!transactional.map(Calibration{0, -1, 1, 1}, 0));
    CHECK(!transactional.map(Calibration{0, 0, 0, 1}, 0));
    CHECK(!transactional.map(Calibration{0, 0, 1, 0}, 0));
    CHECK(transactional.map(identity, 0)->revision == 1);
    CHECK(transactional.map(Calibration{0, 1, 1, 1}, 0)->revision == 2);

    // Inject a finite revision budget to exercise the same pre-increment
    // exhaustion branch as UINT64_MAX without needing 2^64 mappings.
    CaptureClockMapper limited(2);
    CHECK(limited.map(identity, 0)->revision == 1);
    CHECK(limited.map(first, 90'000'000)->revision == 2);
    CHECK(!limited.map(changed, 90'000'000));
    CHECK(!limited.map(identity, 0));
    const auto at_limit = limited.map(first, 90'000'000);
    CHECK(at_limit && at_limit->revision == 2 && at_limit->calibration == first);
    CHECK(at_limit->capture_ns == a->capture_ns);
    CHECK(!limited.map(changed, std::numeric_limits<std::uint64_t>::max()));
    CHECK(limited.map(first, 90'000'000) == at_limit);
    bool threw = false;
    try { CaptureClockMapper invalid(0); } catch (const std::invalid_argument&) { threw = true; }
    CHECK(threw);
}

void test_local_clock_domain()
{
    CHECK(!verify_local_monotonic_domain(nullptr).valid);
    auto* local = GST_CLOCK(g_object_new(GST_TYPE_SYSTEM_CLOCK, "clock-type", GST_CLOCK_TYPE_MONOTONIC, nullptr));
    CHECK(local);
    CHECK(!client_calibration(local));
    CaptureClockMapper mapper;
    CHECK(!mapper.map(local, 0)); // Wrong clock type is rejected without network I/O.
    CHECK(mapper.map(Calibration{0, 0, 1, 1}, 0)->revision == 1);
    CHECK(!verify_local_monotonic_domain(local, -1).valid);
#if defined(_WIN32) || defined(__linux__)
    const auto domain = verify_local_monotonic_domain(local);
    CHECK(domain.valid);
    CHECK(domain.bracket_width_ns >= 0);
    CHECK(domain.bracket_width_ns <= 1'000'000);
    // Calibration changes get_time, not the raw local get_internal_time domain.
    gst_clock_set_calibration(local, 0, GST_SECOND, 1, 1);
    CHECK(verify_local_monotonic_domain(local).valid);
#endif
    g_object_set(local, "clock-type", GST_CLOCK_TYPE_REALTIME, nullptr);
    CHECK(!verify_local_monotonic_domain(local).valid);
    gst_object_unref(local);
}
} // namespace

int main(int argc, char** argv)
{
    gst_init(&argc, &argv);
    try {
        test_mapping();
        test_capture_mapper();
        test_health();
        test_local_clock_domain();
        std::cout << "Network clock tests passed: " << checks << " checks; no network or capture.\n";
        gst_deinit();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Network clock test failed: " << error.what() << '\n';
        gst_deinit();
        return 1;
    }
}
