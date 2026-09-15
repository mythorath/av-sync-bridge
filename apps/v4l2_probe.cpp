// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded, explicit metadata-only observation. Never reads or stores pixels.
#include "avsync/v4l2_capture.hpp"

#include <charconv>
#include <csignal>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
volatile std::sig_atomic_t interrupted = 0;
void stop(int) { interrupted = 1; }

void help()
{
    std::cout << "Usage: avsync-v4l2-probe --device /dev/videoN --capture --seconds 1..120\n"
                 "       avsync-v4l2-probe --help\n"
                 "Explicitly captures metadata from the current progressive NV12 format.\n"
                 "Stop the device's existing owner before capture; this tool never stops it.\n"
                 "No format/rate changes, pixel reads, recordings, playback or network output.\n"
                 "Help/argument errors never open a device. SIGINT/SIGTERM stop cleanly.\n";
}

struct Options { std::string device; int seconds{}; bool capture{}; };

Options parse(int argc, char **argv)
{
    Options result;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        if (argument == "--capture" && !result.capture) {
            result.capture = true;
        } else if (argument == "--device" && result.device.empty() && index + 1 < argc) {
            result.device = argv[++index];
        } else if (argument == "--seconds" && !result.seconds && index + 1 < argc) {
            const std::string_view value(argv[++index]);
            const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result.seconds);
            if (error != std::errc{} || end != value.data() + value.size() ||
                result.seconds < 1 || result.seconds > 120)
                throw std::invalid_argument("seconds must be an integer in 1..120");
        } else {
            throw std::invalid_argument("unknown, repeated or incomplete argument");
        }
    }
    if (!result.capture || result.device.empty() || !result.seconds)
        throw std::invalid_argument("explicit device, capture and seconds are required");
    return result;
}

void quoted(const std::string &value)
{
    std::cout << '"';
    for (const unsigned char character : value) {
        if (character == '\\' || character == '"') std::cout << '\\' << character;
        else if (character < 0x20) std::cout << '?';
        else std::cout << character;
    }
    std::cout << '"';
}

void report(const avsync::V4l2Capture &capture, bool completed, const std::string &error)
{
    const auto &f = capture.format();
    const auto &s = capture.stats();
    const double cadence = s.accepted > 1 && s.last_capture_ns > s.first_capture_ns ?
        static_cast<double>(s.accepted - 1) * 1e9 /
        static_cast<double>(s.last_capture_ns - s.first_capture_ns) : 0.0;
    std::cout << std::boolalpha << std::fixed << std::setprecision(6)
              << "{\n  \"capture_completed\": " << completed << ",\n  \"error\": ";
    quoted(error);
    std::cout << ",\n  \"format\": {\"pixel_format\": \"NV12\", \"width\": " << f.width
              << ", \"height\": " << f.height << ", \"stride\": " << f.stride
              << ", \"size_image\": " << f.size_image << ", \"field\": " << f.field
              << ", \"colorspace\": " << f.colorspace << ", \"ycbcr_encoding\": " << f.ycbcr_encoding
              << ", \"quantization\": " << f.quantization << ", \"transfer_function\": " << f.transfer_function
              << ", \"extended_color_fields_valid\": " << f.extended_color_fields_valid
              << ", \"frame_interval_numerator\": " << f.frame_interval_numerator
              << ", \"frame_interval_denominator\": " << f.frame_interval_denominator << "},\n"
              << "  \"allocated_buffers\": " << s.allocated_buffers
              << ",\n  \"dequeued\": " << s.dequeued << ",\n  \"accepted\": " << s.accepted
              << ",\n  \"driver_error_buffers\": " << s.driver_error_buffers
              << ",\n  \"invalid_payloads\": " << s.invalid_payloads
              << ",\n  \"invalid_timestamps\": " << s.invalid_timestamps
              << ",\n  \"timestamp_flag_changes\": " << s.timestamp_flag_changes
              << ",\n  \"sequence_gaps\": " << s.sequence_gaps
              << ",\n  \"missing_sequences\": " << s.missing_sequences
              << ",\n  \"chronology_errors\": " << s.chronology_errors
              << ",\n  \"io_errors\": " << s.io_errors
              << ",\n  \"cleanup_errors\": " << s.cleanup_errors
              << ",\n  \"poll_timeouts\": " << s.poll_timeouts
              << ",\n  \"callback_errors\": " << s.callback_errors
              << ",\n  \"timestamp_flags\": " << s.timestamp_flags
              << ",\n  \"timestamp_source_flags\": " << s.timestamp_source_flags
              << ",\n  \"driver_advertised_timestamp_point\": \""
              << (s.accepted ? (s.timestamp_source_flags ? "start_of_exposure" : "end_of_frame") : "unobserved")
              << "\",\n  \"observed_accepted_fps\": " << cadence
              << ",\n  \"minimum_interval_ns\": " << s.minimum_interval_ns
              << ",\n  \"maximum_interval_ns\": " << s.maximum_interval_ns
              << ",\n  \"minimum_dequeue_age_ns\": " << s.minimum_dequeue_age_ns
              << ",\n  \"maximum_dequeue_age_ns\": " << s.maximum_dequeue_age_ns
              << ",\n  \"maximum_callback_ns\": " << s.maximum_callback_ns
              << ",\n  \"first_dequeue_elapsed_ms\": " << static_cast<double>(s.first_dequeue_elapsed_ns) / 1e6
              << ",\n  \"first_accepted_elapsed_ms\": " << static_cast<double>(s.first_accepted_elapsed_ns) / 1e6
              << ",\n  \"last_accepted_elapsed_ms\": " << static_cast<double>(s.last_accepted_elapsed_ns) / 1e6
              << ",\n  \"first_accepted_sequence\": " << s.first_accepted_sequence
              << ",\n  \"last_accepted_sequence\": " << s.last_accepted_sequence
              << ",\n  \"last_driver_error_elapsed_ms\": " << static_cast<double>(s.last_driver_error_elapsed_ns) / 1e6
              << ",\n  \"maximum_interval_end_elapsed_ms\": " << static_cast<double>(s.maximum_interval_end_elapsed_ns) / 1e6
              << ",\n  \"error_buffer_events\": [";
    for (std::uint32_t index = 0; index < s.error_buffer_events_recorded; ++index) {
        const auto &event = s.error_buffer_events[index];
        std::cout << (index ? ",\n    {" : "\n    {") << "\"sequence\": " << event.sequence
                  << ", \"flags\": " << event.flags << ", \"bytes_used\": " << event.bytes_used
                  << ", \"dequeue_elapsed_ms\": " << static_cast<double>(event.dequeue_elapsed_ns) / 1e6
                  << ", \"capture_elapsed_ms\": ";
        if (event.capture_timestamp_valid)
            std::cout << static_cast<double>(event.capture_elapsed_ns) / 1e6;
        else
            std::cout << "null";
        std::cout << '}';
    }
    std::cout << "\n  ],\n  \"error_buffer_events_omitted\": "
              << (s.driver_error_buffers - s.error_buffer_events_recorded)
              << ",\n  \"interrupted\": " << s.interrupted << "\n}\n";
}
} // namespace

int main(int argc, char **argv)
{
    // Help wins over every other argument and cannot reach device I/O.
    for (int index = 1; index < argc; ++index)
        if (std::string_view(argv[index]) == "--help") { help(); return 0; }
    if (argc == 1) { help(); return 0; }
    Options options;
    try { options = parse(argc, argv); }
    catch (const std::exception &error) {
        std::cerr << "Argument error: " << error.what() << '\n';
        help();
        return 2;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    std::unique_ptr<avsync::V4l2Capture> capture;
    bool completed = false;
    std::string error;
    try {
        capture = std::make_unique<avsync::V4l2Capture>(options.device);
        capture->run(std::chrono::seconds(options.seconds),
            [](const avsync::V4l2FrameView &) {}, // Deliberately never inspect pixel bytes.
            [] { return interrupted != 0; });
        completed = true;
    } catch (const std::exception &caught) {
        error = caught.what();
    }
    if (!capture) {
        std::cout << "{\"capture_completed\": false, \"error\": ";
        quoted(error);
        std::cout << "}\n";
        return 2;
    }
    report(*capture, completed, error);
    const auto &s = capture->stats();
    const bool clean = completed && s.accepted >= 2 && !s.driver_error_buffers &&
        !s.invalid_payloads && !s.invalid_timestamps && !s.timestamp_flag_changes &&
        !s.missing_sequences && !s.chronology_errors && !s.io_errors && !s.cleanup_errors &&
        !s.callback_errors && !s.interrupted;
    return clean ? 0 : 3;
}
