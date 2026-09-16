// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/timing.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <stdexcept>
#include <vector>

namespace avsync {

// Opt-in diagnostics for progressive, linear BT.709 limited-range NV12.
// One capture-thread writer; inspect only after capture has stopped. Storage is
// allocated and initialized at construction. append never allocates, waits or
// performs I/O, and retains no borrowed pixels. Counts are sparse samples, not
// resized image areas or proof of physical A/V synchronization.
class Nv12MarkerTrace {
public:
    struct Point {
        std::int64_t capture_ns{}, presentation_ns{};
        std::uint32_t sequence{}, cyan_samples{}; // Original V4L2 sequence.
        bool handoff_accepted{};
    };
    static constexpr std::size_t maximum_points = 21'601; // 180 seconds at 120 Hz + one.
    static constexpr std::size_t maximum_payload_bytes = 32U * 1024U * 1024U;
    static constexpr unsigned sampled_pixels = 80 * 45;

    Nv12MarkerTrace(std::uint32_t width, std::uint32_t height, std::uint32_t stride,
                    std::int64_t delay_ns, std::size_t capacity = maximum_points)
        : width_(width), height_(height), stride_(stride), delay_ns_(delay_ns)
    {
        if (!width || !height || width > 3840 || height > 2160 || width % 2 || height % 2 ||
            stride < width || stride % 2 || delay_ns < 0 || delay_ns > 2'000'000'000 ||
            !capacity || capacity > maximum_points)
            throw std::invalid_argument("unsupported NV12 marker trace configuration");
        const auto bytes = std::uint64_t(stride) * (std::uint64_t(height) + height / 2);
        if (bytes > maximum_payload_bytes)
            throw std::invalid_argument("NV12 marker trace payload exceeds bound");
        required_bytes_ = static_cast<std::size_t>(bytes);
        points_.resize(capacity);
    }

    [[nodiscard]] bool append(std::span<const std::byte> pixels, std::int64_t capture_ns,
                              std::uint32_t sequence, bool handoff_accepted) noexcept
    {
        if (invalid_ || overflow_) return false;
        if (count_ == points_.size()) { overflow_ = true; return false; }
        const auto presentation = checked_add(capture_ns, delay_ns_);
        if (!pixels.data() || pixels.size() < required_bytes_ || pixels.size() > maximum_payload_bytes ||
            capture_ns < 0 || !presentation) {
            invalid_ = true;
            return false;
        }
        unsigned cyan = 0;
        // Normalized centers match the OBS smoke trace's x/y=4,12,... at
        // 640x360. Padded input rows and the UV plane use the original stride.
        for (unsigned gy = 0; gy < 45; ++gy) {
            const auto y = (std::size_t(2 * gy + 1) * height_) / 90;
            const auto* luma = pixels.data() + y * stride_;
            const auto* chroma = pixels.data() + (height_ + y / 2) * stride_;
            for (unsigned gx = 0; gx < 80; ++gx) {
                const auto x = (std::size_t(2 * gx + 1) * width_) / 160;
                const double luminance = (std::to_integer<unsigned>(luma[x]) - 16.0) * (255.0 / 219.0);
                const double cb = (std::to_integer<unsigned>(chroma[x & ~std::size_t{1}]) - 128.0) * (255.0 / 224.0);
                const double cr = (std::to_integer<unsigned>(chroma[(x & ~std::size_t{1}) + 1]) - 128.0) * (255.0 / 224.0);
                const double red = std::clamp(luminance + 1.5748 * cr, 0.0, 255.0);
                const double green = std::clamp(luminance - 0.187324 * cb - 0.468124 * cr, 0.0, 255.0);
                const double blue = std::clamp(luminance + 1.8556 * cb, 0.0, 255.0);
                if (green > 125 && blue > 115 && green - red > 60 && blue - red > 50 &&
                    std::abs(green - blue) < 60) ++cyan;
            }
        }
        points_[count_++] = {capture_ns, *presentation, sequence, cyan, handoff_accepted};
        return true;
    }

    [[nodiscard]] std::span<const Point> points() const noexcept { return {points_.data(), count_}; }
    [[nodiscard]] std::size_t capacity() const noexcept { return points_.size(); }
    [[nodiscard]] bool overflow() const noexcept { return overflow_; }
    [[nodiscard]] bool invalid() const noexcept { return invalid_; }

private:
    std::size_t width_{}, height_{}, stride_{}, required_bytes_{}, count_{};
    std::int64_t delay_ns_{};
    std::vector<Point> points_;
    bool overflow_{}, invalid_{};
};

} // namespace avsync
