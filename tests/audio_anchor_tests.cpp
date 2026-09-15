// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/audio_anchors.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
using namespace avsync;
using S = AudioAnchorStatus;
constexpr SessionToken epoch{17, 1};
constexpr auto maximum = std::numeric_limits<std::uint64_t>::max();
constexpr auto ns_maximum = std::numeric_limits<Nanoseconds>::max();
constexpr auto ns_minimum = std::numeric_limits<Nanoseconds>::min();
std::size_t checks = 0;

void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template<class F> void invalid_argument(F&& operation)
{
    bool thrown = false;
    try { operation(); } catch (const std::invalid_argument&) { thrown = true; }
    CHECK(thrown);
}

AudioCaptureAnchor anchor(std::uint64_t sequence = 0, std::uint64_t position = 0,
                         Nanoseconds time = 0, std::uint32_t rate = 48'000)
{
    return {epoch, sequence, position, time, rate, false};
}

void exact_nominal_conversion()
{
    CHECK(!nominal_wire_position(1, 0, 0, 0));
    CHECK(!nominal_wire_position(0, 1, 0, 48'000));
    CHECK((nominal_wire_position(1, 0, 0, 44'100) == ExactWirePosition{1, 3'900, 44'100}));
    CHECK((nominal_wire_position(147, 0, 0, 44'100) == ExactWirePosition{160, 0, 44'100}));
    CHECK((nominal_wire_position(294, 147, 321, 44'100) == ExactWirePosition{481, 0, 44'100}));
    // A per-packet floor of 147 one-frame input blocks would incorrectly be 147.
    std::uint64_t rounded_per_packet = 0;
    for (std::uint64_t i = 0; i < 147; ++i)
        rounded_per_packet += nominal_wire_position(1, 0, 0, 44'100)->whole;
    CHECK(rounded_per_packet == 147);
    CHECK(nominal_wire_position(147, 0, 0, 44'100)->whole == 160);
    for (const std::uint32_t rate : {8'000U, 44'100U, 48'000U, 96'000U, 192'000U}) {
        for (std::uint64_t position = 0; position < 100'000; position += 97) {
            const auto actual = nominal_wire_position(position + 19, 19, 83, rate);
            const auto numerator = position * 48'000;
            CHECK(actual.has_value());
            CHECK(actual->whole == 83 + numerator / rate);
            CHECK(actual->remainder == numerator % rate);
            CHECK(actual->denominator == rate);
        }
    }
    CHECK((nominal_wire_position(maximum, 0, 0, 48'000) == ExactWirePosition{maximum, 0, 48'000}));
    CHECK((nominal_wire_position(maximum, 0, 0, 96'000) == ExactWirePosition{maximum / 2, 48'000, 96'000}));
    CHECK((nominal_wire_position(maximum / 2, 0, 0, 24'000) == ExactWirePosition{maximum - 1, 0, 24'000}));
    CHECK(!nominal_wire_position(maximum / 2 + 1, 0, 0, 24'000));
    CHECK(!nominal_wire_position(1, 0, maximum, 48'000));
    CHECK(nominal_wire_position(maximum, maximum - 1, maximum - 1, 48'000)->whole == maximum);
    constexpr std::uint64_t large_rate = std::numeric_limits<std::uint32_t>::max();
    CHECK((nominal_wire_position(large_rate * large_rate, 0, 0, static_cast<std::uint32_t>(large_rate)) ==
        ExactWirePosition{large_rate * 48'000, 0, static_cast<std::uint32_t>(large_rate)}));
}

void independent_drift_and_nominal_blindness()
{
    constexpr Nanoseconds origin = 8'000'000'123;
    constexpr std::uint64_t device_origin = (std::uint64_t{1} << 63U) + 123;
    for (const std::uint32_t source_rate : {44'100U, 48'000U, 96'000U}) {
        for (const int ppm : {-500, -100, 0, 100, 500}) {
            AudioAnchorConfig config;
            config.rate.nominal_rate = source_rate;
            // Integer-nanosecond source timestamps can quantize a boundary
            // estimate slightly above exactly 500 ppm. Keep raw diagnostics.
            config.rate.max_correction_ppm = 501;
            config.device_frame_origin = device_origin;
            config.wire_frame_origin = 700'000;
            AudioAnchorTracker jittered(epoch, config);
            AudioAnchorTracker unjittered(epoch, config);
            SampleRateEstimator nominal(epoch);
            const RationalTimeline manufactured_pts(origin, 48'000);
            std::uint64_t position = 0;
            std::uint64_t sequence = 0;
            std::uint32_t random_state = 7;
            unsigned measurements = 0;
            unsigned nominal_measurements = 0;
            // Independent continuous-time oracle: not RationalTimeline, wire
            // timestamps, estimator output, or accumulated packet durations.
            const long double actual_rate = static_cast<long double>(source_rate) *
                (1.0L + static_cast<long double>(ppm) / 1'000'000.0L);
            while (position <= static_cast<std::uint64_t>(source_rate) * 31) {
                const auto capture = origin + static_cast<Nanoseconds>(
                    std::floor(static_cast<long double>(position) * 1'000'000'000.0L / actual_rate));
                auto point = anchor(sequence, device_origin + position, capture, source_rate);
                random_state = random_state * 1'664'525U + 1'013'904'223U;
                const auto result = jittered.observe(point, capture + random_state % 100'000'000U);
                const auto reference = unjittered.observe(point, capture);
                CHECK(result.wire_position.has_value());
                CHECK(result.status == reference.status);
                CHECK(result.wire_position == reference.wire_position);
                CHECK(result.wire_position->whole == 700'000 + position * 48'000 / source_rate);
                CHECK(result.wire_position->remainder == position * 48'000 % source_rate);
                CHECK(jittered.latest() == point);
                if (result.estimate) {
                    ++measurements;
                    CHECK(std::abs(result.estimate->source_rate_error_ppm - ppm) < 0.002);
                    CHECK(result.estimate->source_rate_error_ppm == reference.estimate->source_rate_error_ppm);
                    CHECK(result.estimate->within_correction_limit);
                    CHECK(jittered.current_estimate(capture).has_value());
                }
                // This represents the lost-observability failure: using wire
                // sample count to manufacture time erases oscillator drift.
                const auto wire = result.wire_position->whole - 700'000;
                const auto nominal_result = nominal.observe(epoch, wire, *manufactured_pts.at(wire));
                if (nominal_result.estimate) {
                    ++nominal_measurements;
                    CHECK(std::abs(nominal_result.estimate->source_rate_error_ppm) < 0.002);
                }
                ++sequence;
                position += 1 + random_state % 1'920U;
            }
            CHECK(measurements >= 29);
            CHECK(nominal_measurements >= 29);
            CHECK(jittered.diagnostics().accepted == sequence);
            CHECK(jittered.diagnostics().rejected == 0);
        }
    }
}

void faults_and_explicit_resets()
{
    AudioAnchorTracker tracker(epoch);
    const auto first = anchor();
    CHECK(tracker.observe(first, 0).status == S::priming);
    auto other = anchor(1, 480, 10'000'000);
    other.epoch.generation = 2;
    CHECK(tracker.observe(other, other.capture_ns).status == S::wrong_epoch);
    CHECK(!tracker.faulted());
    CHECK(tracker.latest() == first);
    CHECK(tracker.observe(anchor(1, 480, 10'000'000), 10'000'000).status == S::waiting);
    // A missing metadata sequence is not permission to invent an anchor.
    CHECK(tracker.observe(anchor(3, 960, 20'000'000), 20'000'000).status == S::discontinuity);
    CHECK(tracker.faulted());
    CHECK(tracker.latest()->sequence == 1);
    CHECK(tracker.observe(anchor(2, 960, 20'000'000), 20'000'000).status == S::requires_reset);
    CHECK(!tracker.reset(epoch, {}));
    CHECK(!tracker.reset({17, 0}, {}));
    CHECK(tracker.faulted());
    AudioAnchorConfig changed;
    changed.rate.nominal_rate = 44'100;
    changed.device_frame_origin = 123;
    changed.wire_frame_origin = 789;
    CHECK(tracker.reset({17, 2}, changed));
    CHECK(!tracker.faulted() && !tracker.latest());
    auto restarted = anchor(0, 123, 80, 44'100);
    restarted.epoch = {17, 2};
    CHECK(tracker.observe(restarted, 80).wire_position->whole == 789);
    CHECK(tracker.diagnostics().accepted == 3);
    CHECK(tracker.diagnostics().rejected == 3);
    CHECK(tracker.diagnostics().discontinuities == 1);
    CHECK(tracker.diagnostics().wrong_epoch == 1);
    CHECK(tracker.diagnostics().resets == 1);

    const std::array<AudioCaptureAnchor, 7> invalid_next{
        anchor(0, 480, 10'000'000), // Duplicate sequence.
        anchor(2, 480, 10'000'000), // Sequence gap.
        anchor(1, 0, 10'000'000),   // Repeated position.
        anchor(1, 480, 0),         // Repeated time.
        anchor(1, 480, 2'000'000'001), // Gap beyond configured health limit.
        anchor(1, 480, 10'000'000, 44'100), // Rate change.
        AudioCaptureAnchor{epoch, 1, 480, 10'000'000, 48'000, true}
    };
    for (const auto& point : invalid_next) {
        AudioAnchorTracker candidate(epoch);
        CHECK(candidate.observe(first, 0).status == S::priming);
        CHECK(candidate.observe(point, point.capture_ns).status == S::discontinuity);
        CHECK(candidate.latest() == first);
        CHECK(candidate.faulted());
        CHECK(!candidate.current_estimate(point.capture_ns));
    }
    AudioAnchorTracker exhausted(epoch);
    CHECK(exhausted.observe(anchor(maximum, 100, 0), 0).status == S::priming);
    CHECK(exhausted.observe(anchor(0, 101, 1), 1).status == S::discontinuity);
    AudioAnchorTracker backwards(epoch);
    CHECK(backwards.observe(anchor(2, 100, 0), 0).status == S::priming);
    CHECK(backwards.observe(anchor(1, 99, 1), 1).status == S::discontinuity);
}

void health_and_numeric_faults()
{
    AudioAnchorTracker tracker(epoch);
    CHECK(!tracker.fresh(0));
    CHECK(!tracker.current_estimate(0));
    CHECK(tracker.observe(anchor(), 0).status == S::priming);
    CHECK(tracker.observe(anchor(1, 48'000, 1'000'000'000), 1'000'000'000).status == S::measured);
    CHECK(tracker.current_estimate(1'250'000'000).has_value());
    CHECK(!tracker.current_estimate(1'250'000'001));
    CHECK(!tracker.fresh(998'999'999));
    CHECK(tracker.fresh(999'000'000));
    CHECK(tracker.observe(anchor(2, 48'480, 1'010'000'000), 1'260'000'001).status == S::stale);
    CHECK(!tracker.fresh(1'010'000'000));
    CHECK(tracker.diagnostics().stale == 1);
    CHECK(tracker.latest()->sequence == 1);

    AudioAnchorTracker future(epoch);
    CHECK(future.observe(anchor(0, 0, 1'000'001), 0).status == S::future);
    CHECK(future.diagnostics().future == 1);
    AudioAnchorTracker zero_rate(epoch);
    CHECK(zero_rate.observe(anchor(0, 0, 0, 0), 0).status == S::invalid);
    AudioAnchorTracker negative_capture(epoch);
    CHECK(negative_capture.observe(anchor(0, 0, ns_minimum), ns_maximum).status == S::invalid);
    AudioAnchorTracker negative_now(epoch);
    CHECK(negative_now.observe(anchor(), -1).status == S::invalid);
    AudioAnchorTracker large_time(epoch);
    CHECK(large_time.observe(anchor(0, 0, ns_maximum - 1), ns_maximum).status == S::priming);
    CHECK(large_time.observe(anchor(1, 1, ns_maximum), ns_maximum).status == S::waiting);
    CHECK(!large_time.fresh(-1));
    AudioAnchorTracker backwards_time(epoch);
    CHECK(backwards_time.observe(anchor(0, 0, 10), 10).status == S::priming);
    CHECK(backwards_time.observe(anchor(1, 1, 9), 10).status == S::discontinuity);
    AudioAnchorConfig conversion;
    conversion.wire_frame_origin = maximum;
    AudioAnchorTracker wire_overflow(epoch, conversion);
    CHECK(wire_overflow.observe(anchor(0, 1), 0).status == S::overflow);
    conversion.wire_frame_origin = 0;
    conversion.device_frame_origin = 1;
    AudioAnchorTracker pre_origin(epoch, conversion);
    CHECK(pre_origin.observe(anchor(), 0).status == S::invalid);
    AudioAnchorTracker out_of_range(epoch);
    CHECK(out_of_range.observe(anchor(), 0).status == S::priming);
    const auto measurement = out_of_range.observe(anchor(1, 48'048, 1'000'000'000), 1'000'000'000);
    CHECK(measurement.status == S::measured);
    CHECK(std::abs(measurement.estimate->source_rate_error_ppm - 1'000.0) < 1e-6);
    CHECK(!measurement.estimate->within_correction_limit);
    CHECK(!out_of_range.current_estimate(1'000'000'000));

    invalid_argument([] { AudioAnchorTracker invalid({0, 1}); });
    invalid_argument([] { AudioAnchorConfig c; c.max_anchor_age_ns = 0; AudioAnchorTracker invalid(epoch, c); });
    invalid_argument([] { AudioAnchorConfig c; c.max_future_ns = -1; AudioAnchorTracker invalid(epoch, c); });
    invalid_argument([] { AudioAnchorConfig c; c.rate.nominal_rate = 0; AudioAnchorTracker invalid(epoch, c); });
    AudioAnchorConfig invalid;
    invalid.rate.min_window = 0;
    invalid_argument([&] { (void)tracker.reset({17, 2}, invalid); });
    CHECK(tracker.epoch() == epoch);
    CHECK(tracker.faulted());
    CHECK(tracker.latest()->sequence == 1);
}
} // namespace

int main()
{
    try {
        exact_nominal_conversion();
        independent_drift_and_nominal_blindness();
        faults_and_explicit_resets();
        health_and_numeric_faults();
        std::cout << checks << " audio anchor checks passed (synthetic metadata only)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio anchor test failure: " << error.what() << '\n';
        return 1;
    }
}
