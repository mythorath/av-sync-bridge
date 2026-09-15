// SPDX-License-Identifier: GPL-2.0-or-later
// Generated, accelerated conversion fixture. Never opens a device or socket.
#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/gst.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>

namespace {
constexpr std::uint64_t input_rate = 192000, output_rate = 48000;
constexpr std::uint64_t origin_ns = 10'000'000'123;
constexpr std::uint64_t device_origin = 1'234'567;
constexpr std::uint64_t input_bytes_per_frame = 8 * sizeof(float);
constexpr std::uint64_t queue_bytes = input_rate * input_bytes_per_frame / 10;
using Steady = std::chrono::steady_clock;

struct Arguments { unsigned seconds{}; int ppm{}; bool varied{}; };

template<class T> bool integer(std::string_view text, T &value)
{
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

std::optional<Arguments> parse(int argc, char **argv)
{
    Arguments args;
    bool offline = false, seconds = false, ppm = false, pattern = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--offline" && !offline) { offline = true; continue; }
        if (i + 1 == argc) return {};
        const std::string_view value(argv[++i]);
        if (name == "--seconds" && !seconds) {
            seconds = integer(value, args.seconds);
            if (!seconds || args.seconds < 1 || args.seconds > 300) return {};
        } else if (name == "--ppm" && !ppm) {
            ppm = integer(value, args.ppm);
            if (!ppm || args.ppm < -500 || args.ppm > 500) return {};
        } else if (name == "--chunk-pattern" && !pattern) {
            if (value != "fixed" && value != "varied") return {};
            args.varied = value == "varied";
            pattern = true;
        } else return {};
    }
    if (!offline || !seconds || !ppm) return {};
    return args;
}

void require(bool condition, const char *stage)
{
    if (!condition) throw std::runtime_error(stage);
}

// The independent source oscillator produces device frames faster/slower than
// its nominal rate. gst_util_uint64_scale avoids the intermediate product's
// uint64 overflow; the bounded result is less than 301 seconds here.
std::uint64_t source_elapsed(std::uint64_t frames, int ppm)
{
    return gst_util_uint64_scale(frames, 1'000'000'000'000'000ULL,
                                input_rate * static_cast<std::uint64_t>(1'000'000 + ppm));
}

std::uint64_t nominal_elapsed(std::uint64_t frames)
{
    return gst_util_uint64_scale(frames, GST_SECOND, output_rate);
}

struct PipelineDelete {
    void operator()(GstElement *pipeline) const noexcept
    {
        if (pipeline) {
            gst_element_set_state(pipeline, GST_STATE_NULL);
            gst_object_unref(pipeline);
        }
    }
};
using Pipeline = std::unique_ptr<GstElement, PipelineDelete>;

struct BoundaryEvent {
    std::uint64_t output_index{}, pts_ns{};
    std::int64_t step_ns{};
};

struct Statistics {
    int ppm{};
    std::uint64_t output_buffers{}, output_frames{}, initial_discont{}, later_discont{};
    std::uint64_t first_pts{}, last_pts{}, last_duration{}, previous_index{};
    std::uint64_t segment_origin{}, segment_index{}, interval_frames{}, interval_ns{};
    std::uint64_t maximum_nominal_segment_error_ns{}, maximum_ideal_anchor_error_ns{};
    std::uint64_t duration_errors{}, pts_backwards{}, invalid_buffers{}, offset_discontinuities{};
    std::uint64_t last_offset_end{GST_BUFFER_OFFSET_NONE};
    std::uint64_t phase_steps{};
    std::array<BoundaryEvent, 16> discontinuities{}, steps{};
    unsigned recorded_discontinuities{}, recorded_steps{};
    bool negotiated{};
    std::atomic<bool> invalid{false};
};

std::uint64_t distance(std::uint64_t a, std::uint64_t b) noexcept
{
    return a >= b ? a - b : b - a;
}

// fakesink calls this in the streaming thread; it never retains PCM. Statistics
// are read by the control thread only after EOS and pipeline shutdown join it.
void observe(GstElement *, GstBuffer *buffer, GstPad *pad, gpointer opaque) noexcept
{
    auto &s = *static_cast<Statistics *>(opaque);
    if (!s.negotiated) {
        GstCaps *caps = gst_pad_get_current_caps(pad);
        GstAudioInfo info;
        gst_audio_info_init(&info);
        const bool valid = caps && gst_audio_info_from_caps(&info, caps) &&
            GST_AUDIO_INFO_FORMAT(&info) == GST_AUDIO_FORMAT_S24BE &&
            GST_AUDIO_INFO_RATE(&info) == static_cast<gint>(output_rate) &&
            GST_AUDIO_INFO_CHANNELS(&info) == 2 &&
            GST_AUDIO_INFO_LAYOUT(&info) == GST_AUDIO_LAYOUT_INTERLEAVED;
        if (caps) gst_caps_unref(caps);
        if (!valid) { s.invalid.store(true); return; }
        s.negotiated = true;
    }
    const auto bytes = gst_buffer_get_size(buffer);
    const auto pts = GST_BUFFER_PTS(buffer);
    const auto duration = GST_BUFFER_DURATION(buffer);
    if (!bytes || bytes % 6 || !GST_CLOCK_TIME_IS_VALID(pts) ||
        !GST_CLOCK_TIME_IS_VALID(duration) || pts > 400 * GST_SECOND || duration > GST_SECOND ||
        s.output_frames > 15'000'000) {
        ++s.invalid_buffers;
        s.invalid.store(true);
        return;
    }
    const auto frames = static_cast<std::uint64_t>(bytes / 6);
    const bool discont = GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DISCONT);
    const bool first = s.output_buffers == 0;
    const auto step_ns = first ? 0 : static_cast<std::int64_t>(pts) -
                                     static_cast<std::int64_t>(s.last_pts + s.last_duration);
    const bool phase_step = !first && (step_ns > 1 || step_ns < -1);
    if (first || phase_step) {
        s.segment_origin = pts;
        s.segment_index = s.output_frames;
    }
    if (first) {
        s.first_pts = pts;
        if (discont) ++s.initial_discont;
    } else {
        if (pts < s.last_pts) ++s.pts_backwards;
        if (discont) {
            ++s.later_discont;
            if (s.recorded_discontinuities < s.discontinuities.size())
                s.discontinuities[s.recorded_discontinuities++] = {s.output_frames, pts, step_ns};
        }
        if (phase_step) {
            ++s.phase_steps;
            if (s.recorded_steps < s.steps.size())
                s.steps[s.recorded_steps++] = {s.output_frames, pts, step_ns};
        } else if (pts > s.last_pts) {
            s.interval_frames += s.output_frames - s.previous_index;
            s.interval_ns += pts - s.last_pts;
        }
        if (s.last_offset_end != GST_BUFFER_OFFSET_NONE && GST_BUFFER_OFFSET(buffer) != GST_BUFFER_OFFSET_NONE &&
            GST_BUFFER_OFFSET(buffer) != s.last_offset_end) ++s.offset_discontinuities;
    }
    const auto expected_nominal = s.segment_origin + nominal_elapsed(s.output_frames - s.segment_index);
    s.maximum_nominal_segment_error_ns = std::max(s.maximum_nominal_segment_error_ns,
                                                 distance(pts, expected_nominal));
    // This is an ideal fixed-ratio sample ledger, not proof of sample provenance.
    // Once output PTS steps, its PCM association must be considered unknown.
    if (!s.phase_steps) {
        const auto independent = origin_ns + source_elapsed(s.output_frames * 4, s.ppm);
        s.maximum_ideal_anchor_error_ns = std::max(s.maximum_ideal_anchor_error_ns,
                                                  distance(pts, independent));
    }
    if (distance(duration, nominal_elapsed(frames)) > 1) ++s.duration_errors;
    s.previous_index = s.output_frames;
    s.output_frames += frames;
    ++s.output_buffers;
    s.last_pts = pts;
    s.last_duration = duration;
    s.last_offset_end = GST_BUFFER_OFFSET_END(buffer);
}

GstElement *make(GstElement *pipeline, const char *factory, const char *name)
{
    auto *element = gst_element_factory_make(factory, name);
    require(element != nullptr, "missing_conversion_element");
    if (!gst_bin_add(GST_BIN(pipeline), element)) {
        gst_object_unref(element);
        throw std::runtime_error("add_conversion_element");
    }
    return element;
}

void set_mix(GstElement *element)
{
    constexpr float k = 0.7071067811865475f;
    constexpr float gain = 0.9f / (1 + 3 * k);
    constexpr std::array<std::array<float, 8>, 2> rows{{
        {gain, 0, gain*k, 0, gain*k, 0, gain*k, 0},
        {0, gain, gain*k, 0, 0, gain*k, 0, gain*k}}};
    GValue matrix = G_VALUE_INIT;
    g_value_init(&matrix, GST_TYPE_ARRAY);
    for (const auto &values : rows) {
        GValue row = G_VALUE_INIT;
        g_value_init(&row, GST_TYPE_ARRAY);
        for (const auto coefficient : values) {
            GValue value = G_VALUE_INIT;
            g_value_init(&value, G_TYPE_FLOAT);
            g_value_set_float(&value, coefficient);
            gst_value_array_append_value(&row, &value);
            g_value_unset(&value);
        }
        gst_value_array_append_value(&matrix, &row);
        g_value_unset(&row);
    }
    g_object_set_property(G_OBJECT(element), "mix-matrix", &matrix);
    g_value_unset(&matrix);
}

bool check_bus(GstBus *bus)
{
    bool eos = false;
    for (unsigned i = 0; i < 64; ++i) {
        GstMessage *message = gst_bus_pop(bus);
        if (!message) break;
        const auto type = GST_MESSAGE_TYPE(message);
        gst_message_unref(message);
        require(type != GST_MESSAGE_ERROR, "gstreamer_pipeline_error");
        if (type == GST_MESSAGE_EOS) eos = true;
    }
    return eos;
}

int run(const Arguments &args)
{
    Statistics stats;
    stats.ppm = args.ppm;
    std::uint64_t input_frames = 0, input_buffers = 0, last_input_index = 0, last_input_pts = 0;
    std::uint64_t maximum_queue = 0;
    const auto started = Steady::now();
    try {
        require(gst_init_check(nullptr, nullptr, nullptr), "gstreamer_initialize");
        Pipeline pipeline(gst_pipeline_new("offline-conversion-observability"));
        require(pipeline != nullptr, "create_pipeline");
        auto *source = make(pipeline.get(), "appsrc", "generated-audio");
        auto *mix = make(pipeline.get(), "audioconvert", "explicit-stereo-mix");
        auto *float_caps = make(pipeline.get(), "capsfilter", "stereo-float");
        auto *resample = make(pipeline.get(), "audioresample", "current-fixed-conversion");
        auto *quantize = make(pipeline.get(), "audioconvert", "pcm24-convert");
        auto *wire_caps = make(pipeline.get(), "capsfilter", "wire-format");
        auto *sink = make(pipeline.get(), "fakesink", "metadata-only-output");
        g_object_set(source, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", FALSE,
                     "block", FALSE, "max-time", static_cast<guint64>(100 * GST_MSECOND),
                     "max-bytes", static_cast<guint64>(queue_bytes), "max-buffers", static_cast<guint64>(32),
                     "emit-signals", FALSE, nullptr);
        GstCaps *caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
            "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 8,
            "rate", G_TYPE_INT, static_cast<gint>(input_rate), "channel-mask", GST_TYPE_BITMASK,
            static_cast<guint64>(0xc3f), nullptr);
        require(caps != nullptr, "input_caps");
        gst_app_src_set_caps(GST_APP_SRC(source), caps);
        gst_caps_unref(caps);
        set_mix(mix);
        caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
            "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 2,
            "rate", G_TYPE_INT, static_cast<gint>(input_rate), "channel-mask", GST_TYPE_BITMASK,
            static_cast<guint64>(3), nullptr);
        require(caps != nullptr, "float_caps");
        g_object_set(float_caps, "caps", caps, nullptr);
        gst_caps_unref(caps);
        caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S24BE",
            "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 2,
            "rate", G_TYPE_INT, static_cast<gint>(output_rate), "channel-mask", GST_TYPE_BITMASK,
            static_cast<guint64>(3), nullptr);
        require(caps != nullptr, "wire_caps");
        g_object_set(wire_caps, "caps", caps, nullptr);
        gst_caps_unref(caps);
        g_object_set(resample, "quality", 10, "resample-method", GST_AUDIO_RESAMPLER_METHOD_KAISER,
                     "sinc-filter-mode", GST_AUDIO_RESAMPLER_FILTER_MODE_AUTO, nullptr);
        g_object_set(quantize, "dithering", 0, "noise-shaping", 0, nullptr);
        g_object_set(sink, "sync", FALSE, "async", FALSE, "signal-handoffs", TRUE,
                     "enable-last-sample", FALSE, nullptr);
        g_signal_connect(sink, "handoff", G_CALLBACK(observe), &stats);
        require(gst_element_link_many(source, mix, float_caps, resample, quantize, wire_caps, sink, nullptr),
                "link_pipeline");
        std::unique_ptr<GstBus, decltype(&gst_object_unref)> bus(gst_element_get_bus(pipeline.get()), gst_object_unref);
        require(bus != nullptr, "pipeline_bus");
        require(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "pipeline_playing");
        constexpr std::array<std::uint64_t, 8> varied{1, 383, 1919, 1921, 960, 4, 3840, 127};
        const auto total_frames = static_cast<std::uint64_t>(args.seconds) * input_rate;
        const auto deadline = started + std::chrono::seconds(90);
        bool sent_eos = false, received_eos = false;
        while (!received_eos) {
            require(Steady::now() < deadline, "wall_deadline");
            require(!stats.invalid.load(), "invalid_output_metadata");
            received_eos = check_bus(bus.get());
            if (received_eos) break;
            if (input_frames == total_frames) {
                if (!sent_eos) {
                    require(gst_app_src_end_of_stream(GST_APP_SRC(source)) == GST_FLOW_OK, "send_eos");
                    sent_eos = true;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            const auto frames = std::min(args.varied ? varied[input_buffers % varied.size()] : 1920,
                                         total_frames - input_frames);
            const auto bytes = frames * input_bytes_per_frame;
            const auto level = gst_app_src_get_current_level_bytes(GST_APP_SRC(source));
            const auto buffers = gst_app_src_get_current_level_buffers(GST_APP_SRC(source));
            maximum_queue = std::max(maximum_queue, static_cast<std::uint64_t>(level));
            if (level + bytes > queue_bytes || buffers >= 31) {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                continue;
            }
            GstBuffer *buffer = gst_buffer_new_allocate(nullptr, static_cast<gsize>(bytes), nullptr);
            require(buffer != nullptr, "generated_buffer_allocation");
            // No GAP flag: process actual generated zeros through the same DSP.
            gst_buffer_memset(buffer, 0, 0, static_cast<gsize>(bytes));
            const auto pts = origin_ns + source_elapsed(input_frames, args.ppm);
            GST_BUFFER_PTS(buffer) = pts;
            GST_BUFFER_DTS(buffer) = pts;
            GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(frames, GST_SECOND, input_rate);
            GST_BUFFER_OFFSET(buffer) = device_origin + input_frames;
            GST_BUFFER_OFFSET_END(buffer) = device_origin + input_frames + frames;
            if (!input_buffers) GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DISCONT);
            require(gst_app_src_push_buffer(GST_APP_SRC(source), buffer) == GST_FLOW_OK, "push_generated_audio");
            last_input_index = input_frames;
            last_input_pts = pts;
            input_frames += frames;
            ++input_buffers;
        }
        // Join the streaming thread before reading its non-atomic diagnostics.
        pipeline.reset();
        const bool valid = sent_eos && stats.negotiated && stats.output_buffers > 1 && !stats.invalid.load() &&
                           !stats.invalid_buffers && !stats.duration_errors;
        const double original_ppm = (static_cast<double>(last_input_index) * GST_SECOND /
            static_cast<double>(last_input_pts - origin_ns) / input_rate - 1.0) * 1e6;
        const double segment_ppm = stats.interval_ns ? (static_cast<double>(stats.interval_frames) * GST_SECOND /
            static_cast<double>(stats.interval_ns) / output_rate - 1.0) * 1e6 : 0;
        const bool hidden = args.ppm != 0 && std::abs(original_ppm - segment_ppm) > 5.0;
        guint major, minor, micro, nano;
        gst_version(&major, &minor, &micro, &nano);
        std::cout.precision(12);
        std::cout << "{\"schema\":1,\"fixture\":\"offline_current_sender_conversion\",\"fixture_pass\":"
                  << (valid ? "true" : "false") << ",\"gate_zero_pass\":false,\"gate_scope\":\"output_pts_without_original_anchor_transport\""
                  << ",\"observation\":\"" << (hidden ? "nonzero_drift_hidden_within_segments" :
                      (args.ppm == 0 ? "zero_drift_control_only" : "inconclusive")) << '"'
                  << ",\"gstreamer_version\":\"" << major << '.' << minor << '.' << micro << '.' << nano << '"'
                  << ",\"input_rate\":" << input_rate << ",\"output_rate\":" << output_rate
                  << ",\"source_nominal_seconds\":" << args.seconds << ",\"injected_ppm\":" << args.ppm
                  << ",\"chunk_pattern\":\"" << (args.varied ? "varied" : "fixed") << '"'
                  << ",\"input_buffers\":" << input_buffers << ",\"input_frames\":" << input_frames
                  << ",\"original_anchor_elapsed_ns\":" << last_input_pts - origin_ns
                  << ",\"original_anchor_frame_delta\":" << last_input_index
                  << ",\"original_anchor_ppm\":" << original_ppm << ",\"within_output_segment_ppm\":" << segment_ppm
                  << ",\"output_buffers\":" << stats.output_buffers << ",\"output_frames\":" << stats.output_frames
                  << ",\"nominal_output_frames\":" << input_frames / 4
                  << ",\"output_count_difference\":" << static_cast<std::int64_t>(stats.output_frames) - static_cast<std::int64_t>(input_frames / 4)
                  << ",\"first_output_pts_ns\":" << stats.first_pts << ",\"last_output_pts_ns\":" << stats.last_pts
                  << ",\"output_end_pts_ns\":" << stats.last_pts + stats.last_duration
                  << ",\"independent_source_end_ns\":" << origin_ns + source_elapsed(input_frames, args.ppm)
                  << ",\"initial_discontinuities\":" << stats.initial_discont
                  << ",\"later_discontinuities\":" << stats.later_discont
                  << ",\"output_phase_steps\":" << stats.phase_steps
                  << ",\"pts_backwards\":" << stats.pts_backwards
                  << ",\"output_offset_discontinuities\":" << stats.offset_discontinuities
                  << ",\"max_within_segment_nominal_error_ns\":" << stats.maximum_nominal_segment_error_ns
                  << ",\"max_ideal_anchor_error_before_phase_step_ns\":" << stats.maximum_ideal_anchor_error_ns
                  << ",\"individual_sample_identity\":\"unproven\",\"generated_signal\":\"zero_pcm_without_gap_flag\""
                  << ",\"max_observed_appsrc_bytes\":" << maximum_queue << ",\"appsrc_byte_cap\":" << queue_bytes
                  << ",\"wall_ms\":" << std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now() - started).count()
                  << ",\"first_discontinuity_events\":[";
        const auto events = [](const auto &items, unsigned count) {
            for (unsigned i = 0; i < count; ++i) {
                const auto &event = items[i];
                if (i) std::cout << ',';
                std::cout << "{\"output_index\":" << event.output_index << ",\"pts_ns\":" << event.pts_ns
                          << ",\"step_from_previous_end_ns\":" << event.step_ns << '}';
            }
        };
        events(stats.discontinuities, stats.recorded_discontinuities);
        std::cout << "],\"first_phase_step_events\":[";
        events(stats.steps, stats.recorded_steps);
        std::cout << "]}\n";
        return valid && (args.ppm == 0 || hidden) ? 0 : 3;
    } catch (const std::exception &error) {
        std::cout << "{\"schema\":1,\"fixture_pass\":false,\"gate_zero_pass\":false,\"error_stage\":\""
                  << error.what() << "\"}\n";
        return 1;
    }
}

void help()
{
    std::cout << "avsync-conversion-timing-probe --offline --seconds 1..300 --ppm -500..500 [--chunk-pattern fixed|varied]\n"
                 "Accelerated generated 192 kHz eight-channel PCM through the current fixed sender conversion.\n"
                 "Default/help performs no initialization. Explicit runs use no devices, sockets, playback or PCM files.\n"
                 "At most 300 nominal source seconds, 90 wall seconds, 100 ms/32 buffers of source queue.\n"
                 "Exit 0 means the fixture completed, NOT that gate zero or live A/V synchronization passed.\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) { help(); return 0; }
    const auto args = parse(argc, argv);
    if (!args) {
        std::cout << "{\"schema\":1,\"fixture_pass\":false,\"error_stage\":\"arguments\"}\n";
        return 2;
    }
    return run(*args);
}
