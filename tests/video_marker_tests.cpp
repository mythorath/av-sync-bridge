// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/video_marker_trace.hpp"

#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
unsigned checks{};
void check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
using Trace = avsync::Nv12MarkerTrace;

std::vector<std::byte> image(unsigned width, unsigned height, unsigned stride,
                             unsigned y, unsigned u, unsigned v) {
    std::vector<std::byte> pixels(std::size_t(stride) * (height + height / 2), std::byte{0xee});
    for (unsigned row = 0; row < height; ++row)
        for (unsigned x = 0; x < width; ++x) pixels[std::size_t(row) * stride + x] = std::byte(y);
    for (unsigned row = height; row < height + height / 2; ++row)
        for (unsigned x = 0; x < width; x += 2) {
            pixels[std::size_t(row) * stride + x] = std::byte(u);
            pixels[std::size_t(row) * stride + x + 1] = std::byte(v);
        }
    return pixels;
}

bool bad_configuration(unsigned width, unsigned height, unsigned stride, std::int64_t delay = 0,
                       std::size_t capacity = 1) {
    try { Trace trace(width, height, stride, delay, capacity); return false; }
    catch (const std::invalid_argument&) { return true; }
}

void test_colors_and_metadata() {
    Trace trace(2, 2, 2, 2'000'000'000, 3);
    auto pixels = image(2, 2, 2, 16, 128, 128);
    CHECK(trace.append(pixels, 123, 7, true));
    const auto* storage = trace.points().data();
    CHECK(trace.points()[0].cyan_samples == 0);
    CHECK(trace.points()[0].capture_ns == 123);
    CHECK(trace.points()[0].presentation_ns == 2'000'000'123);
    CHECK(trace.points()[0].sequence == 7 && trace.points()[0].handoff_accepted);
    pixels = image(2, 2, 2, 188, 154, 16); // Limited-range BT.709 cyan.
    CHECK(trace.append(pixels, 456, 8, false));
    CHECK(trace.points()[1].cyan_samples == Trace::sampled_pixels);
    CHECK(!trace.points()[1].handoff_accepted);
    pixels = image(2, 2, 2, 235, 128, 128); // White must not pass cyan thresholds.
    CHECK(trace.append(pixels, 789, std::numeric_limits<std::uint32_t>::max(), true));
    CHECK(trace.points()[2].cyan_samples == 0);
    CHECK(trace.points()[1].cyan_samples == Trace::sampled_pixels); // No borrowed pixels.
    CHECK(trace.points().data() == storage && trace.capacity() == 3);
    CHECK(!trace.invalid() && !trace.overflow());
    CHECK(!trace.append(pixels, 999, 0, true));
    CHECK(trace.overflow() && !trace.invalid() && trace.points().size() == 3);
    CHECK(trace.points().data() == storage && trace.points()[2].capture_ns == 789);
}

void test_padding_grid_and_dimensions() {
    auto padded = image(4, 4, 8, 16, 128, 128);
    // Left half cyan; right half black. Row padding remains an unrelated value.
    for (unsigned y = 0; y < 4; ++y)
        for (unsigned x = 0; x < 2; ++x) padded[y * 8 + x] = std::byte{188};
    for (unsigned y = 4; y < 6; ++y) {
        padded[y * 8] = std::byte{154}; padded[y * 8 + 1] = std::byte{16};
    }
    Trace half(4, 4, 8, 0, 1);
    CHECK(half.append(padded, 0, 0, true));
    CHECK(half.points()[0].cyan_samples == Trace::sampled_pixels / 2);

    auto grid = image(640, 360, 640, 16, 128, 128);
    grid[4 * 640 + 4] = std::byte{188};
    grid[(360 + 2) * 640 + 4] = std::byte{154};
    grid[(360 + 2) * 640 + 5] = std::byte{16};
    Trace exact_grid(640, 360, 640, 0, 1);
    CHECK(exact_grid.append(grid, 0, 0, true));
    CHECK(exact_grid.points()[0].cyan_samples == 1);
    Trace largest(3840, 2160, 3840, 0, 1);
    CHECK(largest.append(image(3840, 2160, 3840, 188, 154, 16), 1, 1, true));
    CHECK(largest.points()[0].cyan_samples == Trace::sampled_pixels);

    CHECK(bad_configuration(0, 2, 2));
    CHECK(bad_configuration(2, 0, 2));
    CHECK(bad_configuration(3, 2, 4));
    CHECK(bad_configuration(2, 3, 2));
    CHECK(bad_configuration(3842, 2160, 3842));
    CHECK(bad_configuration(3840, 2162, 3840));
    CHECK(bad_configuration(4, 2, 2));
    CHECK(bad_configuration(2, 2, 3));
    CHECK(bad_configuration(2, 2, std::numeric_limits<unsigned>::max() - 1));
    CHECK(bad_configuration(2, 2, 2, -1));
    CHECK(bad_configuration(2, 2, 2, 2'000'000'001));
    CHECK(bad_configuration(2, 2, 2, 0, 0));
    CHECK(bad_configuration(2, 2, 2, 0, Trace::maximum_points + 1));
    Trace maximum_capacity(2, 2, 2, 0);
    CHECK(maximum_capacity.capacity() == Trace::maximum_points);
    CHECK(Trace::maximum_points == 180U * 120U + 1);
    CHECK(Trace::maximum_points * sizeof(Trace::Point) <= 1024U * 1024U);
}

void test_invalid_payload_and_time() {
    const auto pixels = image(2, 2, 2, 16, 128, 128);
    Trace empty(2, 2, 2, 0, 1);
    CHECK(!empty.append({}, 0, 0, true) && empty.invalid());
    Trace short_pixels(2, 2, 2, 0, 1);
    CHECK(!short_pixels.append(std::span(pixels).first(5), 0, 0, true));
    CHECK(short_pixels.invalid() && short_pixels.points().empty());
    CHECK(!short_pixels.append(pixels, 0, 0, true)); // Invalid trace remains failed closed.
    Trace negative(2, 2, 2, 0, 1);
    CHECK(!negative.append(pixels, -1, 0, true) && negative.invalid());
    Trace overflow(2, 2, 2, 1, 1);
    CHECK(!overflow.append(pixels, std::numeric_limits<std::int64_t>::max(), 0, true));
    CHECK(overflow.invalid() && overflow.points().empty());
    Trace last_timestamp(2, 2, 2, 0, 1);
    CHECK(last_timestamp.append(pixels, std::numeric_limits<std::int64_t>::max(), 0, true));
    CHECK(last_timestamp.points()[0].presentation_ns == std::numeric_limits<std::int64_t>::max());
}
} // namespace

int main() {
    try {
        test_colors_and_metadata();
        test_padding_grid_and_dimensions();
        test_invalid_payload_and_time();
        std::cout << "video marker tests: " << checks << " checks passed; synthetic pixels only\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
