// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/timing.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <utility>

namespace {
using namespace avsync;
constexpr SessionToken epoch{17, 1};
constexpr SessionToken next_epoch{17, 2};
constexpr auto ns_max = std::numeric_limits<Nanoseconds>::max();
constexpr auto ns_min = std::numeric_limits<Nanoseconds>::min();
std::size_t checks = 0;

// Deliberately not assert(): these checks also execute in Release/NDEBUG builds.
void check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) throw std::runtime_error("line " + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

template<class F> void invalid_argument(F&& operation)
{
    bool caught = false;
    try { std::forward<F>(operation)(); }
    catch (const std::invalid_argument&) { caught = true; }
    CHECK(caught);
}

TimedMetadata frame(std::uint64_t sequence, Nanoseconds capture, Nanoseconds presentation,
                    Nanoseconds duration = 10, std::uint64_t bytes = 8, SessionToken token = epoch)
{
    return {token, sequence, capture, presentation, duration, bytes};
}

void arithmetic_and_mapping()
{
    CHECK(checked_add(2, -3) == -1);
    CHECK(checked_add(ns_max, ns_min) == -1);
    CHECK(checked_add(ns_min, 0) == ns_min);
    CHECK(!checked_add(ns_max, 1));
    CHECK(!checked_add(ns_min, -1));
    CHECK(checked_sub(ns_min, ns_min) == 0);
    CHECK(checked_sub(-1, ns_min) == ns_max);
    CHECK(!checked_sub(ns_max, -1));
    CHECK(!checked_sub(ns_min, 1));
    CHECK(!checked_sub(0, ns_min));
    CHECK(valid_successor(epoch, next_epoch));
    CHECK(!valid_successor(epoch, epoch));
    CHECK(!valid_successor(next_epoch, epoch));
    CHECK(!valid_successor(epoch, {0, 1}));
    CHECK(valid_successor(epoch, {18, 1}));

    const CaptureToPresentation mapping(epoch, 2'000'000'000, -50'000'000);
    CHECK(mapping.map(epoch, 25'000'000) == 1'975'000'000);
    CHECK(mapping.map(epoch, -25'000'000) == 1'925'000'000);
    CHECK(!mapping.map(next_epoch, 0));
    CHECK(!mapping.map(epoch, ns_max));
    invalid_argument([] { CaptureToPresentation x(epoch, -1); });
    invalid_argument([] { CaptureToPresentation x(epoch, 0, -1); });
    invalid_argument([] { CaptureToPresentation x(epoch, ns_max, 1); });
    invalid_argument([] { CaptureToPresentation x({}, 1); });
}

void rational_timestamps()
{
    const RationalTimeline video(0, 60'000, 1'001);
    CHECK(video.at(0) == 0);
    CHECK(video.at(1) == 16'683'333);
    CHECK(video.at(2) == 33'366'666);
    CHECK(video.at(3) == 50'050'000);
    CHECK(video.at(60'000) == 1'001'000'000'000);
    CHECK(video.at(60'000ULL * 3'600) == 3'603'600'000'000'000);
    const RationalTimeline audio(-50, 48'000);
    CHECK(audio.at(1) == 20'783);
    CHECK(audio.at(48'000) == 999'999'950);
    CHECK(RationalTimeline(ns_min, 1'000'000'000).at(0) == ns_min);
    CHECK(RationalTimeline(ns_min, 1'000'000'000).at(std::numeric_limits<std::uint64_t>::max()) == ns_max);
    CHECK(!RationalTimeline(1, 1'000'000'000).at(static_cast<std::uint64_t>(ns_max)));
    CHECK(!RationalTimeline(0, 1).at(std::numeric_limits<std::uint64_t>::max()));
    CHECK(RationalTimeline(0, std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint32_t>::max()).at(1) == 1'000'000'000);
    invalid_argument([] { RationalTimeline x(0, 0); });
    invalid_argument([] { RationalTimeline x(0, 1, 0); });
    // Integer reference calculations stay below uint64_t overflow in this grid.
    for (std::uint32_t numerator : {1U, 24U, 60U, 48'000U, 60'000U}) {
        for (std::uint32_t denominator : {1U, 1'001U}) {
            const RationalTimeline timeline(7, numerator, denominator);
            for (std::uint64_t index = 0; index < 1'000; index += 7) {
                const auto reference = static_cast<Nanoseconds>(index * 1'000'000'000ULL * denominator / numerator) + 7;
                CHECK(timeline.at(index) == reference);
            }
        }
    }
}

void queue_limits_and_discontinuities()
{
    BoundedTimelineQueue queue(epoch, {2, 16, 1'000, 100});
    CHECK(queue.push(frame(2, 100, 210), 150) == PushStatus::accepted);
    CHECK(queue.push(frame(1, 90, 200), 150) == PushStatus::accepted);
    CHECK(queue.size() == 2 && queue.bytes() == 16);
    CHECK(queue.push(frame(3, 110, 220), 150) == PushStatus::full);
    CHECK(queue.push(frame(1, 90, 200), 150) == PushStatus::duplicate);
    CHECK(!queue.pop_due(199).item);
    auto first = queue.pop_due(200);
    CHECK(first.item && first.item->sequence == 1);
    CHECK(first.discarded_late.empty());
    CHECK(queue.size() == 1 && queue.bytes() == 8);
    auto expired = queue.pop_due(221, 10);
    CHECK(!expired.item && expired.discarded_late.size() == 1);
    CHECK(expired.discarded_late.front().sequence == 2);
    CHECK(queue.size() == 0 && queue.bytes() == 0);
    CHECK(queue.push(frame(4, 0, 99), 100) == PushStatus::late);
    CHECK(queue.push(frame(5, 0, 1'095), 100) == PushStatus::too_far_future);
    CHECK(queue.push(frame(6, 0, 100, 0), 0) == PushStatus::invalid);
    CHECK(queue.push(frame(6, 0, 100, -1), 0) == PushStatus::invalid);
    CHECK(queue.push(frame(6, 0, 100, 1, 0), 0) == PushStatus::invalid);
    CHECK(queue.push(frame(6, ns_max, 100), 0) == PushStatus::invalid);
    CHECK(queue.push(frame(6, 0, ns_max), 0) == PushStatus::invalid);
    CHECK(queue.push(frame(6, 0, 100, 1, 1, next_epoch), 0) == PushStatus::wrong_epoch);
    CHECK(queue.push(frame(7, 0, 100, 1, 17), 0) == PushStatus::full);
    CHECK(queue.push(frame(8, 0, 100, 101, 1), 0) == PushStatus::full);
    CHECK(queue.push(frame(9, 0, 100), 0) == PushStatus::accepted);
    CHECK(queue.push(frame(10, 0, 191), 0) == PushStatus::full); // 101ns span
    CHECK(!queue.reset(epoch));
    CHECK(queue.size() == 1);
    CHECK(queue.reset(next_epoch));
    CHECK(queue.size() == 0 && queue.bytes() == 0);
    CHECK(queue.push(frame(9, 0, 100), 0) == PushStatus::wrong_epoch);
    CHECK(queue.push(frame(9, 0, 100, 10, 8, next_epoch), 0) == PushStatus::accepted);
    CHECK(queue.pop_due(105, 5).item.has_value());
    invalid_argument([&] { (void)queue.pop_due(0, -1); });
    invalid_argument([] { BoundedTimelineQueue x({}, {}); });
    invalid_argument([] { BoundedTimelineQueue x(epoch, {0, 1, 1, 1}); });
    invalid_argument([] { BoundedTimelineQueue x(epoch, {1, 0, 1, 1}); });
    invalid_argument([] { BoundedTimelineQueue x(epoch, {1, 1, -1, 1}); });
    invalid_argument([] { BoundedTimelineQueue x(epoch, {1, 1, 1, 0}); });

    // Overflow in byte accumulation is rejected before it can wrap.
    const auto max_bytes = std::numeric_limits<std::uint64_t>::max();
    BoundedTimelineQueue huge(epoch, {3, max_bytes, ns_max, ns_max});
    CHECK(huge.push(frame(1, 0, 0, 1, max_bytes), 0) == PushStatus::accepted);
    CHECK(huge.push(frame(2, 0, 1, 1, 1), 0) == PushStatus::full);
    CHECK(huge.bytes() == max_bytes);
    CHECK(huge.pop_due(0).item.has_value());
    CHECK(huge.bytes() == 0);
    CHECK(huge.push(frame(3, 0, 0), ns_min) == PushStatus::too_far_future);
}

void fixed_delay_despite_startup_gap_and_arrival_jitter()
{
    const CaptureToPresentation mapping(epoch, 2'000'000'000);
    const RationalTimeline cadence(0, 60);
    BoundedTimelineQueue queue(epoch, {180, 180 * 8, 3'000'000'000, 3'000'000'000});
    // First frame then a sub-second startup gap: there is intentionally no frame
    // count inference. Subsequent 60fps frames retain exactly the same delay.
    for (std::uint64_t n = 0; n < 120; ++n) {
        if (n > 0 && n < 30) continue;
        const auto capture = *cadence.at(n);
        const auto presentation = *mapping.map(epoch, capture);
        const auto arrival = capture + ((n % 3 == 0) ? 80'000'000 : 1'000'000);
        CHECK(queue.push(frame(n, capture, presentation, 16'666'666), arrival) == PushStatus::accepted);
    }
    CHECK(!queue.pop_due(1'999'999'999).item);
    for (std::uint64_t n = 0; n < 120; ++n) {
        if (n > 0 && n < 30) continue;
        const auto expected = *cadence.at(n) + 2'000'000'000;
        auto output = queue.pop_due(expected);
        CHECK(output.item && output.item->sequence == n);
        CHECK(output.item->presentation_time - output.item->capture_time == 2'000'000'000);
        CHECK(output.discarded_late.empty());
    }
    CHECK(queue.size() == 0);
    // Explicit reorder is sorted by the timestamp, not arrival or sequence.
    CHECK(queue.push(frame(1, 200, 400), 200) == PushStatus::accepted);
    CHECK(queue.push(frame(2, 100, 300), 200) == PushStatus::accepted);
    CHECK(queue.pop_due(300).item->sequence == 2);
    CHECK(queue.pop_due(400).item->sequence == 1);
}

RateEstimate estimate_rate(int ppm, double limit = 500)
{
    SampleRateEstimator estimator(epoch, {48'000, limit, 10'000'000'000, 2'000'000'000});
    CHECK(estimator.observe(epoch, 0, 0).status == RateStatus::priming);
    RateObservation result{RateStatus::waiting, std::nullopt};
    for (std::uint64_t second = 1; second <= 10; ++second) {
        const auto sample = second * static_cast<std::uint64_t>(48'000 * (1'000'000LL + ppm)) / 1'000'000;
        result = estimator.observe(epoch, sample, static_cast<Nanoseconds>(second) * 1'000'000'000);
        if (second < 10) CHECK(result.status == RateStatus::waiting);
    }
    CHECK(result.status == RateStatus::measured && result.estimate);
    return *result.estimate;
}

void independent_sample_rate_estimates()
{
    for (int ppm : {0, 100, -100, 500, -500}) {
        const auto result = estimate_rate(ppm);
        CHECK(std::abs(result.source_rate_error_ppm - ppm) < 1e-6);
        CHECK(result.within_correction_limit);
        CHECK(std::abs(result.output_per_input_ratio - 1.0 / (1.0 + ppm / 1e6)) < 1e-12);
    }
    const auto excessive = estimate_rate(500, 100);
    CHECK(!excessive.within_correction_limit);
    CHECK(excessive.bounded_rate_error_ppm == 100);
    CHECK(excessive.source_rate_error_ppm > 499);
    SampleRateEstimator desktop(epoch), microphone(epoch);
    CHECK(desktop.observe(epoch, 1'000, 0).status == RateStatus::priming);
    CHECK(microphone.observe(epoch, 100, 0).status == RateStatus::priming);
    CHECK(desktop.observe(next_epoch, 2'000, 1).status == RateStatus::wrong_epoch);
    CHECK(desktop.observe(epoch, 49'000, 1'000'000'000).status == RateStatus::measured);
    CHECK(microphone.observe(epoch, 48'124, 1'000'000'000).estimate->source_rate_error_ppm > 499);
    CHECK(desktop.observe(epoch, 1, 2'000'000'000).status == RateStatus::discontinuity);
    CHECK(desktop.observe(epoch, 2, 1'999'999'999).status == RateStatus::discontinuity);
    CHECK(desktop.observe(epoch, 3, 5'000'000'000).status == RateStatus::discontinuity);
    CHECK(desktop.observe(epoch, 3, 5'000'000'001).status == RateStatus::discontinuity);
    CHECK(desktop.observe(epoch, 4, ns_max).status == RateStatus::discontinuity);
    CHECK(desktop.observe(epoch, 5, ns_min).status == RateStatus::discontinuity);
    CHECK(!desktop.reset(epoch));
    CHECK(desktop.reset(next_epoch));
    CHECK(desktop.observe(epoch, 0, 0).status == RateStatus::wrong_epoch);
    CHECK(desktop.observe(next_epoch, 0, 0).status == RateStatus::priming);
    invalid_argument([] { SampleRateEstimator x({}, {}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {0, 500, 1, 1}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {48'000, 0, 1, 1}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {48'000, 1'000'000, 1, 1}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {48'000, std::numeric_limits<double>::quiet_NaN(), 1, 1}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {48'000, 500, 0, 1}); });
    invalid_argument([] { SampleRateEstimator x(epoch, {48'000, 500, 1, 0}); });
}

void microphone_privacy()
{
    MicPrivacyGate gate(epoch);
    CHECK(gate.muted());
    CHECK(!gate.permits(epoch, 0, 10));
    CHECK(gate.set_muted(false, 100));
    CHECK(!gate.permits(epoch, 99, 10)); // Straddling cutoff is not replayed.
    CHECK(gate.permits(epoch, 100, 10));
    CHECK(gate.set_muted(false, 110)); // Idempotent health check preserves cutoff.
    CHECK(gate.permits(epoch, 100, 10));
    CHECK(!gate.set_muted(false, 109));
    CHECK(gate.permits(epoch, 100, 10));
    CHECK(!gate.permits(next_epoch, 100, 10));
    CHECK(!gate.permits(epoch, 100, 0));
    CHECK(!gate.permits(epoch, 100, -1));
    CHECK(!gate.permits(epoch, ns_max, 1));
    CHECK(gate.set_muted(true, 200));
    CHECK(!gate.permits(epoch, 150, 10)); // Already buffered voice stops now.
    CHECK(!gate.permits(epoch, 250, 10));
    CHECK(!gate.set_muted(false, 199)); // Stale command cannot unmute.
    CHECK(gate.muted());
    CHECK(gate.set_muted(false, 300));
    CHECK(!gate.permits(epoch, 150, 10)); // Before mute: never replayed.
    CHECK(!gate.permits(epoch, 250, 10)); // During mute: never replayed.
    CHECK(gate.permits(epoch, 300, 10));
    CHECK(gate.set_muted(false, 400));
    CHECK(gate.permits(epoch, 300, 10));
    CHECK(!gate.set_muted(true, 399));
    CHECK(!gate.muted());
    CHECK(!gate.reset(epoch));
    CHECK(gate.reset(next_epoch));
    CHECK(gate.muted());
    CHECK(!gate.permits(next_epoch, 500, 10));
    CHECK(gate.set_muted(false, 500));
    CHECK(!gate.permits(epoch, 500, 10));
    CHECK(gate.permits(next_epoch, 500, 10));
    invalid_argument([] { MicPrivacyGate x({}); });
}
} // namespace

int main()
{
    try {
        arithmetic_and_mapping();
        rational_timestamps();
        queue_limits_and_discontinuities();
        fixed_delay_despite_startup_gap_and_arrival_jitter();
        independent_sample_rate_estimates();
        microphone_privacy();
        std::cout << "PASS: " << checks << " synthetic timing checks (active in Release)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
