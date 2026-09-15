// SPDX-License-Identifier: GPL-2.0-or-later
// Generated PCM only: no devices, network, playback, OBS, or media files.
#include "avsync/nominal_audio.hpp"
#include <gst/audio/audio.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::uint32_t output_rate = 48000;
constexpr std::array<long double, 6> marker_fractions{0.12L, 0.22L, 0.365L, 0.49L, 0.665L, 0.84L};
void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}
template<class T> T integer(std::string_view text) {
    T value{};
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    require(error == std::errc{} && end == text.data() + text.size(), "invalid integer argument");
    return value;
}
struct Options {
    unsigned seconds{2}, rate{192000}, frequency{1000};
    int ppm{};
    std::string signal{"tone"};
    bool offline{}, shift{};
};
Options parse(int argc, char** argv) {
    Options o; unsigned seen{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--offline" && !o.offline) { o.offline = true; continue; }
        if (name == "--shift-varied-one-sample" && !o.shift) { o.shift = true; continue; }
        require(i + 1 < argc, "missing argument value");
        const std::string_view value(argv[++i]); unsigned bit{};
        if (name == "--seconds") { bit = 1; o.seconds = integer<unsigned>(value); require(o.seconds >= 2 && o.seconds <= 90, "seconds outside 2..90"); }
        else if (name == "--rate") { bit = 2; o.rate = integer<unsigned>(value); require(o.rate == 192000 || o.rate == 44100 || o.rate == 48000, "rate must be 192000, 44100, or 48000"); }
        else if (name == "--frequency") { bit = 4; o.frequency = integer<unsigned>(value); require(o.frequency >= 100 && o.frequency <= 18000, "frequency outside 100..18000"); }
        else if (name == "--ppm") { bit = 8; o.ppm = integer<int>(value); require(o.ppm >= -500 && o.ppm <= 500, "ppm outside -500..500"); }
        else if (name == "--signal") { bit = 16; o.signal = value; require(o.signal == "tone" || o.signal == "markers" || o.signal == "impulse" || o.signal == "silence", "unknown signal"); }
        else throw std::runtime_error("unknown or repeated argument");
        require(!(seen & bit), "repeated argument"); seen |= bit;
    }
    require(o.offline, "explicit --offline required");
    return o;
}
double generated(const Options& o, std::uint64_t index, std::uint32_t rate) {
    const long double time = static_cast<long double>(index) / rate;
    if (o.signal == "silence") return 0;
    if (o.signal == "tone") return static_cast<double>(0.5L * std::sin(2 * std::numbers::pi_v<long double> * o.frequency * time));
    if (o.signal == "impulse") {
        // Deliberately off a rational phase boundary at nonunity rates.
        const auto center = static_cast<std::uint64_t>(std::llround(o.seconds * 0.375L * rate)) + 13;
        return index == center ? 0.5 : 0;
    }
    long double result{};
    for (const auto fraction : marker_fractions) {
        const auto distance = (time - fraction * o.seconds) / 0.001L;
        if (std::abs(distance) < 10) result += 0.5L * std::exp(-0.5L * distance * distance);
    }
    return static_cast<double>(result);
}
struct Run {
    std::vector<float> samples;
    std::uint64_t before_finish{}, calls{};
    std::size_t latency{};
    double max_call_ms{};
};
Run convert(const Options& o, bool varied, Clock::time_point deadline) {
    const unsigned channels = o.rate == 192000 ? 8 : 2;
    const std::array<GstAudioChannelPosition, 8> positions{
        GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT, GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER, GST_AUDIO_CHANNEL_POSITION_LFE1,
        GST_AUDIO_CHANNEL_POSITION_REAR_LEFT, GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT,
        GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT, GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT};
    GstAudioInfo input_info{};
    gst_audio_info_set_format(&input_info, GST_AUDIO_FORMAT_F32, static_cast<int>(o.rate), static_cast<int>(channels), positions.data());
    std::array<std::array<float, 8>, 2> matrix{};
    matrix[0][0] = 1; matrix[1][1] = 1;
    // Selecting the first two channels isolates conversion timing/identity.
    // Surround mixing policy has separate tests; this fixture does not certify it.
    const std::size_t max_input = o.rate / 10;
    avsync::audio::NominalAudioConverter converter(input_info, matrix, max_input);
    Run run; run.latency = converter.max_latency_input_frames();
    const std::uint64_t total_input = static_cast<std::uint64_t>(o.seconds) * o.rate;
    const auto desired = static_cast<std::size_t>(o.seconds) * output_rate;
    run.samples.reserve(desired * 2);
    std::vector<float> input(max_input * channels);
    // Explicit bounds: <=90 seconds stereo retained per run and <=100 ms scratch.
    std::vector<float> output((output_rate / 10 + 4096) * 2);
    constexpr std::array<std::size_t, 9> pattern{1, 7, 53, 211, 509, 997, 31, 1921, 3079};
    std::uint64_t offset{};
    while (offset < total_input) {
        require(Clock::now() < deadline, "generated conversion deadline exceeded");
        const auto requested = varied ? std::min(pattern[run.calls % pattern.size()], max_input) : o.rate / 100;
        const auto frames = static_cast<std::size_t>(std::min<std::uint64_t>(requested, total_input - offset));
        std::fill(input.begin(), input.begin() + static_cast<std::ptrdiff_t>(frames * channels), 0.0F);
        for (std::size_t i = 0; i < frames; ++i) input[i * channels] = static_cast<float>(generated(o, offset + i, o.rate));
        const auto expected = converter.required_output_frames(frames);
        require(expected * 2 <= output.size(), "conversion output exceeds scratch bound");
        const auto start = Clock::now();
        const auto result = converter.process(std::as_bytes(std::span(input.data(), frames * channels)), frames, std::span(output.data(), expected * 2));
        run.max_call_ms = std::max(run.max_call_ms, std::chrono::duration<double, std::milli>(Clock::now() - start).count());
        require(result.status == avsync::audio::NominalAudioStatus::progress || result.status == avsync::audio::NominalAudioStatus::no_progress, "nominal conversion failed");
        require(result.input_frames_used == frames && result.output_frames_generated == expected, "nominal conversion count mismatch");
        require(run.samples.size() + expected * 2 <= desired * 2, "nominal converter produced more than source duration");
        run.samples.insert(run.samples.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(expected * 2));
        offset += frames; ++run.calls;
    }
    run.before_finish = run.samples.size() / 2;
    const auto remaining = converter.required_finish_output_frames();
    require(remaining * 2 <= output.size(), "finish output exceeds scratch bound");
    const auto finish = converter.finish(std::span(output.data(), remaining * 2));
    require(finish.status == avsync::audio::NominalAudioStatus::finished && finish.input_frames_used == 0 && finish.output_frames_generated == remaining, "nominal finish failed");
    require(run.samples.size() + remaining * 2 <= desired * 2, "finish produced more than source duration");
    run.samples.insert(run.samples.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(remaining * 2));
    return run;
}
double db(double value) { return 20 * std::log10(std::max(value, 1e-15)); }
struct Metrics {
    double peak{}, right_peak{}, maximum_error{}, rms{}, gain_db{}, marker_error{}, impulse_error{}, impulse_peak{}, tone_phase_samples{};
    std::uint64_t compared{}, markers{};
};
Metrics inspect(const Options& o, const Run& run) {
    Metrics m;
    long double errors{}, expected_energy{}, observed_energy{}, inphase{}, quadrature{};
    const auto frames = run.samples.size() / 2;
    for (std::size_t i = 0; i < frames; ++i) {
        const double actual = run.samples[i * 2];
        require(std::isfinite(actual) && std::isfinite(run.samples[i * 2 + 1]), "nonfinite output PCM");
        m.peak = std::max(m.peak, std::abs(actual));
        m.right_peak = std::max(m.right_peak, static_cast<double>(std::abs(run.samples[i * 2 + 1])));
        if (o.signal == "impulse" || i < output_rate / 8 || i + output_rate / 8 >= frames) continue;
        const double reference = generated(o, i, output_rate);
        const double error = actual - reference;
        m.maximum_error = std::max(m.maximum_error, std::abs(error));
        errors += error * error; expected_energy += reference * reference;
        observed_energy += actual * actual; ++m.compared;
        if (o.signal == "tone") {
            const auto angle = 2 * std::numbers::pi_v<long double> * o.frequency * i / output_rate;
            inphase += actual * std::sin(angle);
            quadrature += actual * std::cos(angle);
        }
    }
    m.rms = m.compared ? std::sqrt(static_cast<double>(errors / m.compared)) : 0;
    m.gain_db = expected_energy > 0 ? 10 * std::log10(static_cast<double>(observed_energy / expected_energy)) : 0;
    if (o.signal == "tone") m.tone_phase_samples = static_cast<double>(std::atan2(quadrature, inphase) * output_rate / (2 * std::numbers::pi_v<long double> * o.frequency));
    if (o.signal == "markers") {
        for (const auto fraction : marker_fractions) {
            const auto center = static_cast<std::size_t>(std::llround(fraction * o.seconds * output_rate));
            require(center >= 240 && center + 240 < frames, "marker outside output range");
            std::size_t peak_index = center - 240; float peak{};
            for (auto i = center - 240; i <= center + 240; ++i) {
                if (run.samples[i * 2] > peak) { peak = run.samples[i * 2]; peak_index = i; }
            }
            if (peak > 0.45F) ++m.markers;
            m.marker_error = std::max(m.marker_error, std::abs(static_cast<double>(peak_index) - static_cast<double>(center)));
        }
    }
    if (o.signal == "impulse") {
        const auto source_center = static_cast<std::uint64_t>(std::llround(o.seconds * 0.375L * o.rate)) + 13;
        const auto expected_center = static_cast<double>(source_center) * output_rate / o.rate;
        std::size_t peak_index{};
        for (std::size_t i = 0; i < frames; ++i) {
            const auto value = std::abs(static_cast<double>(run.samples[i * 2]));
            if (value > m.impulse_peak) { m.impulse_peak = value; peak_index = i; }
        }
        m.impulse_error = std::abs(static_cast<double>(peak_index) - expected_center);
    }
    return m;
}
void help() {
    std::cout << "avsync-nominal-audio-probe --offline [--seconds 2..90] [--rate 44100|48000|192000]\n"
                 " [--ppm -500..500] [--signal tone|markers|impulse|silence] [--frequency 100..18000]\n"
                 " [--shift-varied-one-sample]\n"
                 "Generated PCM, fixed-versus-varied chunk comparison, no hardware/network/files.\n"
                 "PPM describes separate original capture metadata, not a resampling command.\n"
                 "One-sample shift is a deliberate negative control; it should fail for tone/markers.\n";
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && std::string_view(argv[1]) == "--help") { help(); return 0; }
    try {
        const auto o = parse(argc, argv);
        gst_init(nullptr, nullptr);
        const auto start = Clock::now();
        const auto deadline = start + std::chrono::seconds(110);
        const auto fixed = convert(o, false, deadline);
        auto varied = convert(o, true, deadline);
        if (o.shift && varied.samples.size() >= 4) std::rotate(varied.samples.begin(), varied.samples.begin() + 2, varied.samples.end());
        const auto fixed_metrics = inspect(o, fixed), varied_metrics = inspect(o, varied);
        const auto target = static_cast<std::uint64_t>(o.seconds) * output_rate;
        const bool counts = fixed.samples.size() / 2 == target && varied.samples.size() / 2 == target && fixed.before_finish == varied.before_finish;
        double partition_error{}; std::size_t partition_error_index{};
        const auto comparable = std::min(fixed.samples.size(), varied.samples.size());
        for (std::size_t i = 0; i < comparable; ++i) {
            const auto error = static_cast<double>(std::abs(fixed.samples[i] - varied.samples[i]));
            if (error > partition_error) { partition_error = error; partition_error_index = i / 2; }
        }
        const bool invariant = fixed.samples.size() == varied.samples.size() && partition_error <= 1e-7;
        const auto rms = std::max(fixed_metrics.rms, varied_metrics.rms);
        const auto maximum_error = std::max(fixed_metrics.maximum_error, varied_metrics.maximum_error);
        const auto right = std::max(fixed_metrics.right_peak, varied_metrics.right_peak);
        const auto peak = std::max(fixed_metrics.peak, varied_metrics.peak);
        const auto marker_error = std::max(fixed_metrics.marker_error, varied_metrics.marker_error);
        const auto impulse_error = std::max(fixed_metrics.impulse_error, varied_metrics.impulse_error);
        const bool analytic = o.signal == "impulse" || (db(rms) <= -80 && db(maximum_error) <= -50 && std::abs(fixed_metrics.gain_db) <= 0.2 && std::abs(varied_metrics.gain_db) <= 0.2);
        const bool markers = o.signal != "markers" || (fixed_metrics.markers == 6 && varied_metrics.markers == 6 && marker_error <= 1);
        const bool impulse = o.signal != "impulse" || (impulse_error <= 1 && fixed_metrics.impulse_peak > 0.05 && varied_metrics.impulse_peak > 0.05);
        const bool passed = counts && invariant && analytic && markers && impulse && right <= 1e-7 && peak <= 1 && (o.signal != "silence" || peak <= 1e-7);
        // Analytic metadata only: no clock samples or anchors pass through the
        // converter. Actual transport anchor association is a separate test.
        const auto nominal_ns = static_cast<std::uint64_t>(o.seconds) * GST_SECOND;
        const auto capture_ns = gst_util_uint64_scale(static_cast<std::uint64_t>(o.seconds) * o.rate, 1'000'000'000'000'000ULL, static_cast<std::uint64_t>(o.rate) * static_cast<std::uint64_t>(1'000'000 + o.ppm));
        const auto capture_difference = static_cast<std::int64_t>(capture_ns) - static_cast<std::int64_t>(nominal_ns);
        std::cout << std::setprecision(12) << "{\"schema\":1,\"generated_only\":true,\"timestamps_passed_to_converter\":false,\"capture_metadata_only\":true,\"live_sync_proven\":false"
            << ",\"passed\":" << (passed ? "true" : "false") << ",\"signal\":\"" << o.signal << "\",\"seconds\":" << o.seconds
            << ",\"source_rate\":" << o.rate << ",\"source_channels\":" << (o.rate == 192000 ? 8 : 2) << ",\"frequency_hz\":" << o.frequency << ",\"source_ppm\":" << o.ppm
            << ",\"negative_control\":" << (o.shift ? "true" : "false") << ",\"input_frames\":" << static_cast<std::uint64_t>(o.seconds) * o.rate
            << ",\"fixed_output_frames\":" << fixed.samples.size() / 2 << ",\"varied_output_frames\":" << varied.samples.size() / 2
            << ",\"fixed_before_finish_frames\":" << fixed.before_finish << ",\"varied_before_finish_frames\":" << varied.before_finish
            << ",\"max_latency_input_frames\":" << fixed.latency << ",\"fixed_calls\":" << fixed.calls << ",\"varied_calls\":" << varied.calls
            << ",\"partition_max_error\":" << partition_error << ",\"partition_max_error_frame\":" << partition_error_index
            << ",\"fixed_tone_phase_samples\":" << fixed_metrics.tone_phase_samples << ",\"varied_tone_phase_samples\":" << varied_metrics.tone_phase_samples
            << ",\"residual_rms_dbfs\":" << db(rms) << ",\"maximum_residual_dbfs\":" << db(maximum_error)
            << ",\"fixed_gain_db\":" << fixed_metrics.gain_db << ",\"varied_gain_db\":" << varied_metrics.gain_db << ",\"right_peak_dbfs\":" << db(right)
            << ",\"output_peak\":" << peak << ",\"marker_regions\":" << std::min(fixed_metrics.markers, varied_metrics.markers) << ",\"max_marker_error_samples\":" << marker_error
            << ",\"max_impulse_error_samples\":" << impulse_error << ",\"impulse_peak\":" << std::min(fixed_metrics.impulse_peak, varied_metrics.impulse_peak)
            << ",\"original_capture_duration_ns\":" << capture_ns << ",\"nominal_output_duration_ns\":" << nominal_ns << ",\"capture_minus_nominal_ns\":" << capture_difference
            << ",\"max_conversion_call_ms\":" << std::max(fixed.max_call_ms, varied.max_call_ms) << ",\"elapsed_ms\":" << std::chrono::duration<double, std::milli>(Clock::now() - start).count() << "}\n";
        return passed ? 0 : 3;
    } catch (const std::exception& error) {
        std::cerr << "generated nominal fixture failed: " << error.what() << '\n';
        return 2;
    }
}
