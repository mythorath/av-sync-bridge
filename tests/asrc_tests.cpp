// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/asrc.hpp"

#include <samplerate.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iostream>
#include <limits>
#include <memory>
#include <numbers>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using Converter = avsync::AsrcStereo;
using S = avsync::AsrcStatus;
unsigned checks = 0;
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value)
        throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

std::vector<float> tone(std::size_t frames)
{
    std::vector<float> samples(frames * Converter::channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        samples[2 * frame] = static_cast<float>(0.4 * std::sin(
            2.0 * std::numbers::pi * 1'003.0 * static_cast<double>(frame) / 48'000.0));
        samples[2 * frame + 1] = static_cast<float>(0.25 * std::cos(
            2.0 * std::numbers::pi * 8'017.0 * static_cast<double>(frame) / 48'000.0));
    }
    return samples;
}

std::vector<float> convert(Converter& converter, std::span<const float> input,
                           double ppm, std::size_t input_quantum, std::size_t output_quantum)
{
    std::vector<float> result;
    std::vector<float> output(output_quantum * 2);
    std::size_t offset = 0;
    std::size_t block_end = std::min(input.size(), input_quantum * 2);
    for (std::size_t call = 0; call < 200'000; ++call) {
        const auto pending = input.subspan(offset, block_end - offset);
        const bool eos = block_end == input.size();
        const auto report = converter.process(pending, output, ppm, eos);
        CHECK(report.library_error == 0);
        CHECK(report.status != S::invalid && report.status != S::failed);
        CHECK(report.input_frames_used * 2 <= pending.size());
        CHECK(report.output_frames_generated <= output_quantum);
        result.insert(result.end(), output.begin(),
                      output.begin() + static_cast<std::ptrdiff_t>(report.output_frames_generated * 2));
        offset += report.input_frames_used * 2;
        if (report.status == S::finished) {
            CHECK(offset == input.size());
            return result;
        }
        CHECK(report.status == S::progress);
        if (offset == block_end && offset < input.size())
            block_end = std::min(input.size(), offset + input_quantum * 2);
    }
    throw std::runtime_error("bounded conversion did not finish");
}

void test_validation_and_no_progress()
{
    CHECK(Converter::valid_ppm(-500));
    CHECK(Converter::valid_ppm(500));
    CHECK(Converter::valid_ppm(-0.0));
    CHECK(!Converter::valid_ppm(500.0001));
    CHECK(!Converter::valid_ppm(-500.0001));
    CHECK(!Converter::valid_ppm(std::numeric_limits<double>::infinity()));
    CHECK(!Converter::valid_ppm(std::numeric_limits<double>::quiet_NaN()));
    CHECK(Converter::backend_version() != nullptr);
    bool threw = false;
    try {
        Converter invalid(std::numeric_limits<double>::quiet_NaN());
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    CHECK(threw);

    Converter subject(100), reference(100);
    auto input = tone(1'920);
    std::array<float, 960> output{};
    const auto odd_input = std::span<const float>(input).first(3);
    const auto odd_output = std::span<float>(output).first(3);
    CHECK(subject.process(odd_input, output, 100).status == S::invalid);
    CHECK(subject.process(input, odd_output, 100).status == S::invalid);
    CHECK(subject.process(input, output, 501).status == S::invalid);
    CHECK(subject.process(input, output, -501).status == S::invalid);
    CHECK(subject.process(input, output, std::numeric_limits<double>::quiet_NaN()).status == S::invalid);
    CHECK(subject.process(input, output, std::numeric_limits<double>::infinity()).status == S::invalid);
    CHECK(subject.process(input, std::span<float>(input).first(960), 100).status == S::invalid);
    CHECK(subject.process(std::span<const float>(input).subspan(100, 960),
                          std::span<float>(input).first(960), 100).status == S::invalid);
    CHECK(subject.process(std::span<const float>(input).first(960),
                          std::span<float>(input).subspan(100, 960), 100).status == S::invalid);
    std::vector<float> oversized_input((Converter::max_input_frames + 1) * 2);
    std::vector<float> oversized_output((Converter::max_output_frames + 1) * 2);
    CHECK(subject.process(oversized_input, output, 100).status == S::invalid);
    CHECK(subject.process(input, oversized_output, 100).status == S::invalid);
    const float first = input[0];
    for (float value : {std::numeric_limits<float>::quiet_NaN(),
                        std::numeric_limits<float>::infinity(),
                        -std::numeric_limits<float>::infinity()}) {
        input[0] = value;
        CHECK(subject.process(input, output, 100).status == S::invalid);
    }
    input[0] = first;
    CHECK(!subject.reset(501));
    for (int iteration = 0; iteration < 16; ++iteration) {
        CHECK(subject.process({}, output, -500).status == S::no_progress);
        CHECK(subject.process(input, {}, -500).status == S::no_progress);
        CHECK(subject.process({}, {}, -500, true).status == S::no_progress);
    }
    // Invalid/no-progress calls neither consume history nor retarget smoothing.
    CHECK(convert(subject, input, 100, 480, 480) == convert(reference, input, 100, 480, 480));
}

void test_counts_eos_and_reset()
{
    const auto input = tone(12'000);
    for (const double ppm : {-500.0, -100.0, 0.0, 100.0, 500.0}) {
        Converter converter(ppm);
        const auto output = convert(converter, input, ppm, 1'920, 480);
        const double expected = static_cast<double>(input.size() / 2) / (1.0 + ppm / 1e6);
        CHECK(std::abs(static_cast<double>(output.size() / 2) - expected) <= 1.0);
        CHECK(std::all_of(output.begin(), output.end(), [](float v) { return std::isfinite(v); }));
        std::array<float, 960> scratch{};
        CHECK(converter.process({}, scratch, ppm, true).status == S::finished);
        CHECK(converter.process(input, scratch, ppm).status == S::invalid);
        CHECK(converter.process(std::span<const float>(input).first(2), scratch, ppm, true).status == S::invalid);
        CHECK(converter.reset(ppm));
        CHECK(output == convert(converter, input, ppm, 1'920, 480));
    }

    Converter converter;
    std::array<float, 2> small_output{};
    auto final_input = tone(480);
    const auto first = converter.process(final_input, small_output, 0, true);
    CHECK(first.status == S::progress);
    CHECK(converter.process({}, small_output, 0, false).status == S::invalid);
    CHECK(converter.reset(-100));
    Converter fresh(-100);
    CHECK(convert(converter, input, -100, 17, 31) == convert(fresh, input, -100, 17, 31));

    // No stale nonzero history escapes after repeated explicit generation reset.
    const std::vector<float> silence(960);
    for (int iteration = 0; iteration < 12; ++iteration) {
        CHECK(converter.reset());
        CHECK(converter.process(final_input, small_output, 0).status == S::progress);
        CHECK(converter.reset());
        const auto silent_output = convert(converter, silence, 0, 480, 480);
        CHECK(std::all_of(silent_output.begin(), silent_output.end(), [](float v) { return v == 0.0F; }));
    }

    // A true empty segment finishes, rather than spinning indefinitely.
    CHECK(converter.reset());
    CHECK(converter.process({}, small_output, 0, true).status == S::finished);
}

void test_constant_partial_calls()
{
    const auto input = tone(3'840);
    for (const double ppm : {-500.0, 0.0, 500.0}) {
        Converter baseline(ppm);
        const auto expected = convert(baseline, input, ppm, 1'920, 480);
        for (const std::size_t input_quantum : {1U, 19U, 480U, 1'920U}) {
            for (const std::size_t output_quantum : {1U, 17U, 480U, 3'840U}) {
                Converter partial(ppm);
                const auto actual = convert(partial, input, ppm, input_quantum, output_quantum);
                CHECK(actual.size() == expected.size());
                double maximum_error = 0;
                for (std::size_t i = 0; i < actual.size(); ++i)
                    maximum_error = std::max(maximum_error, std::abs(static_cast<double>(actual[i] - expected[i])));
                CHECK(maximum_error <= 2e-6);
            }
        }
    }
}

void test_raw_backend_partial_smoothing()
{
    Converter converter(500);
    int error = 0;
    std::unique_ptr<SRC_STATE, decltype(&src_delete)> raw(
        src_new(SRC_SINC_BEST_QUALITY, 2, &error), &src_delete);
    CHECK(raw != nullptr && error == 0);
    CHECK(src_set_ratio(raw.get(), 1.0 / 1.0005) == 0);
    auto input = tone(20'000);
    std::array<float, 7'680> actual{};
    std::array<float, 7'680> expected{};
    constexpr std::array<std::size_t, 6> input_sizes{1, 17, 480, 1'919, 89, 3'840};
    constexpr std::array<std::size_t, 6> output_sizes{3'840, 480, 1, 137, 31, 960};
    constexpr std::array<double, 6> targets{-500, 100, -100, 500, 0, -500};
    std::size_t offset = 0;
    float dummy = 0;
    bool finished = false;
    for (std::size_t call = 0; call < 10'000; ++call) {
        const std::size_t available = std::min(input.size() / 2 - offset, input_sizes[call % input_sizes.size()]);
        const auto pending = std::span<const float>(input).subspan(offset * 2, available * 2);
        const auto capacity = output_sizes[call % output_sizes.size()];
        const double ppm = targets[call % targets.size()];
        // Only start EOS after the final data was consumed, making varying
        // partial packet sizes independent of the segment-end state machine.
        const bool eos = offset == input.size() / 2;
        const auto result = converter.process(pending, std::span<float>(actual).first(capacity * 2), ppm, eos);
        SRC_DATA data{};
        data.data_in = pending.empty() ? &dummy : pending.data();
        data.data_out = expected.data();
        data.input_frames = static_cast<long>(available);
        data.output_frames = static_cast<long>(capacity);
        data.src_ratio = 1.0 / (1.0 + ppm / 1e6);
        data.end_of_input = eos ? 1 : 0;
        CHECK(src_process(raw.get(), &data) == 0);
        CHECK(result.status != S::invalid && result.status != S::failed);
        CHECK(result.input_frames_used == static_cast<std::size_t>(data.input_frames_used));
        CHECK(result.output_frames_generated == static_cast<std::size_t>(data.output_frames_gen));
        CHECK(std::equal(actual.begin(), actual.begin() + data.output_frames_gen * 2, expected.begin()));
        offset += result.input_frames_used;
        if (result.status == S::finished) {
            CHECK(eos);
            finished = true;
            break;
        }
        CHECK(result.status == S::progress);
    }
    CHECK(finished);

    // A requested target is not a fixed-duration ramp: different capacities
    // yield measurably different waveforms during the same ratio transition.
    Converter short_call(500), long_call(500);
    auto ramp_input = tone(9'600);
    const auto short_result = short_call.process(ramp_input, std::span<float>(actual).first(960), -500);
    const auto long_result = long_call.process(ramp_input, expected, -500);
    CHECK(short_result.output_frames_generated == 480);
    CHECK(long_result.output_frames_generated == 3'840);
    double difference = 0;
    for (std::size_t i = 0; i < 960; ++i)
        difference = std::max(difference, std::abs(static_cast<double>(actual[i] - expected[i])));
    CHECK(difference > 1e-4);
}

void test_unclipped_finite_input()
{
    Converter converter;
    std::vector<float> input(9'600 * 2, 1.5F);
    const auto output = convert(converter, input, 0, 1'920, 480);
    CHECK(*std::max_element(output.begin(), output.end()) > 1.0F);
    // Gain and true-peak/headroom policy belongs to the caller; wrapper does
    // not turn finite floating point values into clipped fixed-point samples.
}
} // namespace

int main()
{
    try {
        test_validation_and_no_progress();
        test_counts_eos_and_reset();
        test_constant_partial_calls();
        test_raw_backend_partial_smoothing();
        test_unclipped_finite_input();
        std::cout << checks << " ASRC checks passed (" << Converter::backend_version() << ")\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "ASRC test failure: " << error.what() << '\n';
        return 1;
    }
}
