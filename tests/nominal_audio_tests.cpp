// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/nominal_audio.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Converter = avsync::audio::NominalAudioConverter;
using Status = avsync::audio::NominalAudioStatus;
unsigned checks{};
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

GstAudioInfo info(int rate, int channels, GstAudioFormat format = GST_AUDIO_FORMAT_F32)
{
    GstAudioInfo result;
    gst_audio_info_init(&result);
    gst_audio_info_set_format(&result, format, rate, channels, nullptr);
    return result;
}
Converter::Mix stereo_mix()
{
    Converter::Mix result{};
    result[0][0] = result[1][1] = 1.0F;
    return result;
}
template<class F> void throws(F&& action)
{
    bool did_throw{};
    try { action(); } catch (const std::exception&) { did_throw = true; }
    CHECK(did_throw);
}

std::vector<float> run(Converter& converter, std::span<const float> input,
                       std::size_t channels, bool varied)
{
    constexpr std::array<std::size_t, 11> chunks{1, 7, 13, 3, 257, 509, 11, 47, 1, 997, 211};
    std::vector<float> result, output(48'002);
    std::size_t offset{}, step{};
    while (offset < input.size() / channels) {
        const auto frames = std::min({input.size() / channels - offset,
            converter.max_input_frames(), varied ? chunks[step++ % chunks.size()] : std::size_t{480}});
        const auto required = converter.required_output_frames(frames);
        const auto report = converter.process(std::as_bytes(input.subspan(offset * channels, frames * channels)),
                                               frames, output);
        CHECK(report.status == Status::progress);
        CHECK(report.input_frames_used == frames);
        CHECK(report.output_frames_generated == required);
        offset += frames;
        result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(required * 2));
    }
    const auto pending = converter.required_finish_output_frames();
    if (pending) {
        CHECK(converter.finish(std::span(output).first(pending * 2 - 1)).status == Status::invalid);
        CHECK(converter.required_finish_output_frames() == pending);
    }
    const auto finish = converter.finish(output);
    CHECK(finish.status == Status::finished);
    CHECK(finish.input_frames_used == 0);
    CHECK(finish.output_frames_generated == pending);
    result.insert(result.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(pending * 2));
    CHECK(converter.finish({}).status == Status::finished);
    CHECK(converter.finish({}).output_frames_generated == 0);
    CHECK(converter.required_finish_output_frames() == 0);
    CHECK(converter.input_frames_processed() == input.size() / channels);
    CHECK(converter.output_frames_processed() == result.size() / 2);
    return result;
}

void validation()
{
    const auto format = info(48'000, 2);
    const auto matrix = stereo_mix();
    throws([&] { Converter c(format, matrix, 0); });
    throws([&] { Converter c(format, matrix, 24'001); });
    auto bad = format;
    bad.layout = GST_AUDIO_LAYOUT_NON_INTERLEAVED;
    throws([&] { Converter c(bad, matrix, 480); });
    bad = format; bad.bpf = 1;
    throws([&] { Converter c(bad, matrix, 480); });
    bad = format; bad.rate = 7999;
    throws([&] { Converter c(bad, matrix, 480); });
    bad = format; bad.rate = 44'101;
    throws([&] { Converter c(bad, matrix, 480); });
    bad = format; bad.channels = 9;
    throws([&] { Converter c(bad, matrix, 480); });
    auto bad_matrix = matrix;
    bad_matrix[0][2] = 0.1F;
    throws([&] { Converter c(format, bad_matrix, 480); });
    bad_matrix = matrix; bad_matrix[0][0] = std::numeric_limits<float>::quiet_NaN();
    throws([&] { Converter c(format, bad_matrix, 480); });
    bad_matrix = matrix; bad_matrix[0][0] = 1.01F;
    throws([&] { Converter c(format, bad_matrix, 480); });

    Converter converter(format, matrix, 480);
    CHECK(converter.max_input_frames() == 480);
    CHECK(converter.max_latency_input_frames() == 0);
    std::array<float, 960> input{}, output{};
    CHECK(converter.process({}, 0, {}).status == Status::no_progress);
    CHECK(converter.process(std::as_bytes(std::span(input)), 479, output).status == Status::invalid);
    CHECK(converter.process(std::as_bytes(std::span(input)), 480, std::span(output).first(959)).status == Status::invalid);
    CHECK(converter.process(std::as_bytes(std::span(input)), 480, input).status == Status::invalid);
    throws([&] { (void)converter.required_output_frames(481); });
    for (float invalid : {std::numeric_limits<float>::infinity(),
                           -std::numeric_limits<float>::infinity(),
                           std::numeric_limits<float>::quiet_NaN()}) {
        input[4] = invalid;
        CHECK(converter.process(std::as_bytes(std::span(input)), 480, output).status == Status::invalid);
        CHECK(converter.input_frames_processed() == 0);
    }
    input[4] = 0.125F;
    CHECK(converter.process(std::as_bytes(std::span(input)), 480, output).status == Status::progress);
    CHECK(output == input);
    CHECK(converter.finish({}).status == Status::finished);
    CHECK(converter.process(std::as_bytes(std::span(input)), 480, output).status == Status::invalid);
    throws([&] { (void)converter.required_output_frames(1); });
    CHECK(converter.reset());
    CHECK(converter.input_frames_processed() == 0);
    CHECK(converter.output_frames_processed() == 0);
    CHECK(converter.process(std::as_bytes(std::span(input)), 480, output).status == Status::progress);
    CHECK(output == input);
}

void phases_and_counts()
{
    for (int rate : {8'000, 11'025, 44'100, 48'000, 96'000, 192'000, 384'000}) {
        const auto format = info(rate, 2);
        Converter fixed(format, stereo_mix(), static_cast<std::size_t>(rate / 2));
        Converter varied(format, stereo_mix(), static_cast<std::size_t>(rate / 2));
        const auto frames = static_cast<std::size_t>(rate * 2 + 31);
        std::vector<float> input(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            input[i * 2] = static_cast<float>(0.4 * std::sin(2 * std::numbers::pi * 997 * static_cast<double>(i) / rate));
            input[i * 2 + 1] = static_cast<float>(0.3 * std::cos(2 * std::numbers::pi * 431 * static_cast<double>(i) / rate));
        }
        const auto a = run(fixed, input, 2, false);
        const auto b = run(varied, input, 2, true);
        CHECK(a.size() == b.size());
        CHECK(a.size() / 2 == frames * 48'000 / static_cast<std::size_t>(rate));
        double squared{};
        for (std::size_t i = 0; i < a.size(); ++i) {
            CHECK(std::isfinite(a[i]));
            CHECK(std::abs(a[i] - b[i]) < 0.000002F);
            const auto frame = i / 2;
            if (frame > 4800 && frame + 4800 < a.size() / 2) {
                const auto expected = i % 2 ? 0.3 * std::cos(2 * std::numbers::pi * 431 * static_cast<double>(frame) / 48'000) :
                    0.4 * std::sin(2 * std::numbers::pi * 997 * static_cast<double>(frame) / 48'000);
                squared += (a[i] - expected) * (a[i] - expected);
            }
        }
        CHECK(std::sqrt(squared / static_cast<double>(a.size() - 19'200)) < 0.00002);
        CHECK(fixed.reset());
        const auto again = run(fixed, input, 2, true);
        CHECK(a == again);
    }
}

void impulse_mix_and_short_eos()
{
    for (int rate : {8'000, 44'100, 48'000, 192'000, 384'000}) {
        const auto format = info(rate, 8);
        Converter::Mix mix{};
        mix[0][0] = 0.5F; mix[0][2] = 0.25F;
        mix[1][1] = 0.5F; mix[1][2] = 0.25F;
        Converter converter(format, mix, static_cast<std::size_t>(rate / 2));
        std::vector<float> input(static_cast<std::size_t>(rate) * 8);
        // An ignored LFE channel and one centered impulse prove mapping and
        // zero nominal phase independently of the tone comparison.
        for (int i = 0; i < rate; ++i) input[static_cast<std::size_t>(i) * 8 + 3] = 0.9F;
        input[static_cast<std::size_t>(rate / 2) * 8 + 2] = 1.0F;
        const auto result = run(converter, input, 8, true);
        CHECK(result.size() == 96'000);
        std::size_t peak{};
        for (std::size_t i = 0; i < result.size() / 2; ++i) {
            CHECK(result[i * 2] == result[i * 2 + 1]);
            if (std::abs(result[i * 2]) > std::abs(result[peak * 2])) peak = i;
        }
        CHECK(peak == 24'000);
        CHECK(std::abs(result[2 * 1000]) < 0.000001F);
        for (std::size_t length : {std::size_t{1}, std::size_t{2}, std::size_t{7}, std::size_t{31}}) {
            CHECK(converter.reset());
            const std::vector<float> tiny(length * 8, 0.0F);
            const auto tail = run(converter, tiny, 8, true);
            CHECK(tail.size() / 2 == length * 48'000 / static_cast<std::size_t>(rate));
            CHECK(std::all_of(tail.begin(), tail.end(), [](float x) { return x == 0.0F; }));
        }
    }
}

void pcm_formats()
{
    for (auto format : {GST_AUDIO_FORMAT_U8, GST_AUDIO_FORMAT_S8, GST_AUDIO_FORMAT_S16,
                         GST_AUDIO_FORMAT_S24, GST_AUDIO_FORMAT_S32, GST_AUDIO_FORMAT_F32,
                         GST_AUDIO_FORMAT_F64}) {
        const auto source = info(192'000, 2, format);
        Converter converter(source, stereo_mix(), 1920);
        std::vector<std::byte> input(static_cast<std::size_t>(source.bpf) * 1920,
                                     format == GST_AUDIO_FORMAT_U8 ? std::byte{0x80} : std::byte{0});
        std::vector<float> output(2000);
        const auto result = converter.process(input, 1920, output);
        CHECK(result.status == Status::progress);
        CHECK(result.input_frames_used == 1920);
        CHECK(std::all_of(output.begin(), output.begin() + static_cast<std::ptrdiff_t>(result.output_frames_generated * 2),
                          [](float x) { return x == 0.0F; }));
        CHECK(converter.finish(output).status == Status::finished);
        CHECK(converter.output_frames_processed() == 480);
    }
    const auto source = info(48'000, 1, GST_AUDIO_FORMAT_F64);
    Converter::Mix mono{}; mono[0][0] = mono[1][0] = 1.0F;
    Converter converter(source, mono, 480);
    const std::array<double, 1> input{std::numeric_limits<double>::infinity()};
    std::array<float, 2> output{};
    CHECK(converter.process(std::as_bytes(std::span(input)), 1, output).status == Status::invalid);
    CHECK(converter.input_frames_processed() == 0);
}
} // namespace

int main(int argc, char** argv)
{
    gst_init(&argc, &argv);
    try {
        validation();
        phases_and_counts();
        impulse_mix_and_short_eos();
        pcm_formats();
        std::cout << "nominal audio checks passed: " << checks << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "nominal audio test failed: " << error.what() << '\n';
        return 1;
    }
}
