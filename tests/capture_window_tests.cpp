// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/capture_window.hpp"
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {
unsigned checks = 0;
void check(bool value, const char *expression, int line)
{
    ++checks;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
using avsync::capture_window_status;
using avsync::CaptureWindowPolicy;
using S = avsync::CaptureWindowStatus;
constexpr auto maximum = std::numeric_limits<std::int64_t>::max();

void test_boundaries()
{
    CHECK(capture_window_status(0, 0) == S::accepted);
    CHECK(capture_window_status(1'100'000'000, 1'000'000'000) == S::accepted);
    CHECK(capture_window_status(1'100'000'001, 1'000'000'000) == S::too_far_future);
    CHECK(capture_window_status(900'000'000, 1'000'000'000) == S::accepted);
    CHECK(capture_window_status(899'999'999, 1'000'000'000) == S::too_old);
    CHECK(capture_window_status(-1, 0) == S::invalid);
    CHECK(capture_window_status(0, -1) == S::invalid);
    CHECK(capture_window_status(0, 0, {-1, 0}) == S::invalid);
    CHECK(capture_window_status(0, 0, {0, -1}) == S::invalid);
    CHECK(capture_window_status(maximum, maximum) == S::accepted);
    CHECK(capture_window_status(maximum, maximum - 100'000'000) == S::accepted);
    CHECK(capture_window_status(maximum, maximum - 100'000'001) == S::too_far_future);
    CHECK(capture_window_status(maximum - 100'000'000, maximum) == S::accepted);
    CHECK(capture_window_status(maximum - 100'000'001, maximum) == S::too_old);
    CHECK(capture_window_status(maximum, 0) == S::too_far_future);
    CHECK(capture_window_status(0, maximum) == S::too_old);
    CHECK(capture_window_status(maximum, 0, {maximum, maximum}) == S::accepted);
    CHECK(capture_window_status(0, maximum, {maximum, maximum}) == S::accepted);
    CHECK(capture_window_status(10, 10, {0, 0}) == S::accepted);
    CHECK(capture_window_status(11, 10, {0, 0}) == S::too_far_future);
    CHECK(capture_window_status(9, 10, {0, 0}) == S::too_old);
}

void test_same_policy_before_and_after_work()
{
    constexpr auto capture = std::int64_t{1'020'000'000};
    CHECK(capture_window_status(capture, 1'000'000'000) == S::accepted);
    CHECK(capture_window_status(capture, 1'020'000'000) == S::accepted);
    CHECK(capture_window_status(capture, 1'120'000'000) == S::accepted);
    CHECK(capture_window_status(capture, 1'120'000'001) == S::too_old);
    CHECK(capture == 1'020'000'000); // Eligibility never rewrites the packet's PTS.
    constexpr CaptureWindowPolicy asymmetric{10, 20};
    CHECK(capture_window_status(110, 100, asymmetric) == S::accepted);
    CHECK(capture_window_status(111, 100, asymmetric) == S::too_far_future);
    CHECK(capture_window_status(80, 100, asymmetric) == S::accepted);
    CHECK(capture_window_status(79, 100, asymmetric) == S::too_old);
    for (std::int64_t delta = -150'000'000; delta <= 150'000'000; delta += 50'000) {
        const auto expected = delta < -100'000'000 ? S::too_old :
            delta > 100'000'000 ? S::too_far_future : S::accepted;
        CHECK(capture_window_status(1'000'000'000 + delta, 1'000'000'000) == expected);
        CHECK(capture_window_status(maximum - 200'000'000 + delta, maximum - 200'000'000) == expected);
    }
}
} // namespace

int main()
{
    try {
        test_boundaries();
        test_same_policy_before_and_after_work();
        std::cout << "capture-window tests: " << checks << " checks passed; no capture or network\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
