// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace avsync {

// Values in the color/field members are the original Linux V4L2 enum values.
// No defaults are guessed here: a later presentation adapter must resolve them.
struct V4l2Format {
    std::uint32_t width{}, height{}, stride{}, size_image{};
    std::uint32_t pixel_format{}, field{}, colorspace{}, ycbcr_encoding{};
    std::uint32_t quantization{}, transfer_function{};
    std::uint32_t frame_interval_numerator{}, frame_interval_denominator{};
    bool extended_color_fields_valid{};
};

struct V4l2FrameView {
    // Borrowed mapped storage, valid only until the callback returns. An async
    // consumer must copy into bounded owned storage before returning.
    std::span<const std::byte> pixels;
    std::int64_t capture_ns{};
    std::int64_t dequeue_ns{}; // Diagnostic only; never a substitute for capture_ns.
    std::uint32_t sequence{}, flags{}, timestamp_flags{}, timestamp_source_flags{};
};

struct V4l2ErrorBufferEvent {
    std::uint32_t sequence{}, flags{}, bytes_used{};
    std::int64_t dequeue_elapsed_ns{}, capture_elapsed_ns{};
    bool capture_timestamp_valid{};
};

struct V4l2CaptureStats {
    std::uint64_t dequeued{}, accepted{}, driver_error_buffers{}, invalid_payloads{};
    std::uint64_t invalid_timestamps{}, timestamp_flag_changes{}, sequence_gaps{};
    std::uint64_t missing_sequences{}, chronology_errors{}, io_errors{}, cleanup_errors{};
    std::uint64_t poll_timeouts{}, callback_errors{};
    std::uint32_t allocated_buffers{}, timestamp_flags{}, timestamp_source_flags{};
    std::int64_t first_capture_ns{}, last_capture_ns{};
    std::int64_t minimum_interval_ns{}, maximum_interval_ns{};
    std::int64_t minimum_dequeue_age_ns{}, maximum_dequeue_age_ns{};
    std::int64_t maximum_callback_ns{};
    // Relative monotonic times include allocation/STREAMON startup. These are
    // diagnostics only and do not replace the original frame timestamps.
    std::int64_t first_dequeue_elapsed_ns{}, first_accepted_elapsed_ns{}, last_accepted_elapsed_ns{};
    std::int64_t last_driver_error_elapsed_ns{}, maximum_interval_end_elapsed_ns{};
    std::uint32_t first_accepted_sequence{}, last_accepted_sequence{};
    std::array<V4l2ErrorBufferEvent, 16> error_buffer_events{};
    std::uint32_t error_buffer_events_recorded{};
    bool interrupted{};
};

// Linux-only implementation. Opening/querying is separate from streaming;
// run() is explicit, single-use and bounded. No format/rate/control writes, no
// background thread, no device enumeration and no arrival-time rebasing.
class V4l2Capture {
public:
    using FrameCallback = std::function<void(const V4l2FrameView &)>;
    using StopRequested = std::function<bool()>;

    explicit V4l2Capture(const std::string &device);
    ~V4l2Capture();
    V4l2Capture(const V4l2Capture &) = delete;
    V4l2Capture &operator=(const V4l2Capture &) = delete;
    V4l2Capture(V4l2Capture &&) = delete;
    V4l2Capture &operator=(V4l2Capture &&) = delete;

    [[nodiscard]] const V4l2Format &format() const noexcept;
    [[nodiscard]] const V4l2CaptureStats &stats() const noexcept;
    void run(std::chrono::seconds duration, const FrameCallback &callback,
             const StopRequested &stop_requested = {});
    // Explicit finite supervised use; caller must provide its live stop/lease
    // predicate. Same single generation, capture buffers and original timestamps.
    void run_supervised(std::chrono::seconds duration, const FrameCallback &callback,
                        const StopRequested &stop_requested);

private:
    void run_impl(std::chrono::seconds duration, const FrameCallback &callback,
                  const StopRequested &stop_requested);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace avsync
