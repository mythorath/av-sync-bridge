// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/video_timing.hpp"

#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value)
        throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using C = avsync::TimestampClock;
using S = avsync::VideoSequenceStatus;
using T = avsync::VideoTimestampStatus;
using avsync::timestamp_from_timeval;
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
constexpr auto minimum = std::numeric_limits<std::int64_t>::min();

void test_timeval()
{
    static_assert(timestamp_from_timeval(1, 1, C::monotonic) == 1'000'001'000);
    CHECK(timestamp_from_timeval(0, 0, C::monotonic) == 0);
    CHECK(timestamp_from_timeval(0, 999'999, C::monotonic) == 999'999'000);
    CHECK(timestamp_from_timeval(1, 0, C::monotonic) == 1'000'000'000);
    CHECK(!timestamp_from_timeval(1, 0, C::unknown));
    CHECK(!timestamp_from_timeval(1, 0, C::copied));
    CHECK(!timestamp_from_timeval(1, 0, static_cast<C>(255)));
    CHECK(!timestamp_from_timeval(-1, 0, C::monotonic));
    CHECK(!timestamp_from_timeval(0, -1, C::monotonic));
    CHECK(!timestamp_from_timeval(0, 1'000'000, C::monotonic));
    CHECK(!timestamp_from_timeval(minimum, minimum, C::monotonic));
    CHECK(!timestamp_from_timeval(maximum, maximum, C::monotonic));
    CHECK(!timestamp_from_timeval(maximum, 0, C::monotonic));
    CHECK(!timestamp_from_timeval(0, maximum, C::monotonic));
    constexpr auto last_seconds = maximum / 1'000'000'000;
    constexpr auto last_micros = (maximum % 1'000'000'000) / 1'000;
    CHECK(timestamp_from_timeval(last_seconds, last_micros, C::monotonic) ==
          maximum - maximum % 1'000);
    CHECK(!timestamp_from_timeval(last_seconds, last_micros + 1, C::monotonic));
    CHECK(!timestamp_from_timeval(last_seconds + 1, 0, C::monotonic));
    CHECK(timestamp_from_timeval(last_seconds - 1, 999'999, C::monotonic).has_value());
    for (std::int64_t micros = 0; micros < 1'000'000; micros += 997) {
        CHECK(timestamp_from_timeval(123, micros, C::monotonic) == 123'000'000'000 + micros * 1'000);
        const auto near_limit = timestamp_from_timeval(last_seconds, micros, C::monotonic);
        CHECK(near_limit.has_value() == (micros <= last_micros));
    }
}

void test_wrap_and_gaps()
{
    avsync::VideoTimingTracker tracker;
    auto result = tracker.observe(0xffff'fffeU, 10);
    CHECK(result.accepted());
    CHECK(result.sequence_status == S::first);
    CHECK(result.timestamp_status == T::first);
    result = tracker.observe(0xffff'ffffU, 20);
    CHECK(result.accepted() && result.sequence_status == S::normal);
    result = tracker.observe(0, 30);
    CHECK(result.accepted() && result.sequence_status == S::normal);
    CHECK(result.missing_frames == 0);
    result = tracker.observe(5, 100);
    CHECK(result.accepted() && result.sequence_status == S::gap);
    CHECK(result.timestamp_status == T::forward);
    CHECK(result.missing_frames == 4);
    result = tracker.observe(6, 101);
    CHECK(result.accepted() && result.sequence_status == S::normal);
    CHECK(result.missing_frames == 0);

    avsync::VideoTimingTracker wrap_gap;
    CHECK(wrap_gap.observe(0xffff'fffeU, 1).accepted());
    result = wrap_gap.observe(2, 2);
    CHECK(result.accepted() && result.sequence_status == S::gap);
    CHECK(result.missing_frames == 3);
}

void test_rejections_preserve_anchor()
{
    avsync::VideoTimingTracker tracker;
    auto result = tracker.observe(40, -1);
    CHECK(!result.accepted() && result.timestamp_status == T::invalid);
    result = tracker.observe(40, 100);
    CHECK(result.accepted() && result.sequence_status == S::first);

    result = tracker.observe(40, 120);
    CHECK(!result.accepted() && result.sequence_status == S::repeated);
    CHECK(result.timestamp_status == T::forward);
    result = tracker.observe(39, 120);
    CHECK(!result.accepted() && result.sequence_status == S::backwards);
    result = tracker.observe(41, 100);
    CHECK(!result.accepted() && result.timestamp_status == T::repeated);
    result = tracker.observe(41, 99);
    CHECK(!result.accepted() && result.timestamp_status == T::regression);
    result = tracker.observe(41, minimum);
    CHECK(!result.accepted() && result.timestamp_status == T::invalid);
    result = tracker.observe(41, 101);
    CHECK(result.accepted() && result.sequence_status == S::normal);

    // Observe both anomalies, not just whichever check runs first.
    result = tracker.observe(40, 99);
    CHECK(!result.accepted());
    CHECK(result.sequence_status == S::backwards);
    CHECK(result.timestamp_status == T::regression);
    result = tracker.observe(100, 99);
    CHECK(!result.accepted() && result.sequence_status == S::gap);
    CHECK(result.missing_frames == 58);
    result = tracker.observe(42, 102);
    CHECK(result.accepted() && result.sequence_status == S::normal);

    // Nanosecond comparisons do not subtract and cannot overflow.
    result = tracker.observe(43, maximum);
    CHECK(result.accepted() && result.timestamp_status == T::forward);
    result = tracker.observe(44, 0);
    CHECK(!result.accepted() && result.timestamp_status == T::regression);
    result = tracker.observe(44, maximum);
    CHECK(!result.accepted() && result.timestamp_status == T::repeated);

    // A new instance is an explicit new capture generation; no hidden rebase.
    avsync::VideoTimingTracker next_generation;
    result = next_generation.observe(0, 0);
    CHECK(result.accepted() && result.sequence_status == S::first);
}

void test_modular_boundaries()
{
    constexpr std::uint32_t anchors[] = {0, 1, 2, 0x7fff'ffffU, 0x8000'0000U, 0xffff'fffeU, 0xffff'ffffU};
    constexpr std::uint32_t distances[] = {0, 1, 2, 99, 0x7fff'ffffU, 0x8000'0000U, 0x8000'0001U, 0xffff'ffffU};
    for (const auto anchor : anchors) {
        for (const auto distance : distances) {
            avsync::VideoTimingTracker tracker;
            CHECK(tracker.observe(anchor, 0).accepted());
            const auto result = tracker.observe(static_cast<std::uint32_t>(anchor + distance), maximum);
            const auto expected = distance == 0 ? S::repeated : distance == 1 ? S::normal :
                distance < 0x8000'0000U ? S::gap : distance == 0x8000'0000U ? S::ambiguous : S::backwards;
            CHECK(result.sequence_status == expected);
            CHECK(result.accepted() == (distance > 0 && distance < 0x8000'0000U));
            CHECK(result.missing_frames == (expected == S::gap ? distance - 1 : 0));
        }
    }
}

void test_long_progression()
{
    avsync::VideoTimingTracker tracker;
    std::uint32_t sequence = 0xffff'ff00U;
    std::int64_t timestamp = 900'000'000'000;
    CHECK(tracker.observe(sequence, timestamp).accepted());
    for (std::uint32_t index = 1; index <= 10'000; ++index) {
        const auto step = index % 97 == 0 ? 4U : 1U;
        sequence = static_cast<std::uint32_t>(sequence + step);
        timestamp += 16'666'667 * static_cast<std::int64_t>(step);
        const auto result = tracker.observe(sequence, timestamp);
        CHECK(result.accepted());
        CHECK(result.sequence_status == (step == 1 ? S::normal : S::gap));
        CHECK(result.missing_frames == step - 1);
    }
}
} // namespace

int main()
{
    try {
        test_timeval();
        test_wrap_and_gaps();
        test_rejections_preserve_anchor();
        test_modular_boundaries();
        test_long_progression();
        std::cout << "video-timing tests: " << checks << " checks passed; no hardware or media\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
