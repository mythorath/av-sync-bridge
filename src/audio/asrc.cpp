// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/asrc.hpp"

#include <samplerate.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <stdexcept>

namespace avsync {
namespace {
double ratio_from_ppm(double ppm) noexcept { return 1.0 / (1.0 + ppm / 1'000'000.0); }

// uintptr_t comparisons avoid relational comparisons of unrelated arrays.
// Lengths are already capped before this helper, but check address addition.
bool overlaps(std::span<const float> input, std::span<float> output) noexcept
{
    if (input.empty() || output.empty())
        return false;
    const auto in_begin = reinterpret_cast<std::uintptr_t>(input.data());
    const auto out_begin = reinterpret_cast<std::uintptr_t>(output.data());
    const auto maximum = std::numeric_limits<std::uintptr_t>::max();
    if (in_begin > maximum - input.size_bytes() ||
        out_begin > maximum - output.size_bytes())
        return true;
    return in_begin < out_begin + output.size_bytes() &&
           out_begin < in_begin + input.size_bytes();
}
} // namespace

struct AsrcStereo::State {
    SRC_STATE* converter = nullptr;
    bool draining = false;
    bool finished = false;
    bool failed = false;
    // A nonnull input pointer allows libsamplerate's sinc prepare_data to mark
    // end-of-input even when a subsequent drain supplies zero actual frames.
    float empty_input = 0.0F;
    ~State() { src_delete(converter); }
};

AsrcStereo::AsrcStereo(double initial_ppm)
{
    if (!valid_ppm(initial_ppm))
        throw std::invalid_argument("ASRC initial ppm must be finite and within +/-500");
    state_ = std::make_unique<State>();
    int error = 0;
    state_->converter = src_new(SRC_SINC_BEST_QUALITY, static_cast<int>(channels), &error);
    if (state_->converter == nullptr || error != 0)
        throw std::runtime_error("ASRC BEST_QUALITY stereo initialization failed");
    if (src_set_ratio(state_->converter, ratio_from_ppm(initial_ppm)) != 0)
        throw std::runtime_error("ASRC initial ratio initialization failed");
}

AsrcStereo::~AsrcStereo() = default;

bool AsrcStereo::valid_ppm(double ppm) noexcept
{
    return std::isfinite(ppm) && std::abs(ppm) <= max_abs_ppm;
}

const char* AsrcStereo::backend_version() noexcept { return src_get_version(); }

AsrcProcessResult AsrcStereo::process(std::span<const float> input,
                                     std::span<float> output,
                                     double requested_ppm,
                                     bool end_of_input) noexcept
{
    static_assert(max_input_frames <= static_cast<std::size_t>(std::numeric_limits<long>::max()));
    static_assert(max_output_frames <= static_cast<std::size_t>(std::numeric_limits<long>::max()));
    if (!valid_ppm(requested_ppm) || input.size() % channels != 0 ||
        output.size() % channels != 0 || input.size() / channels > max_input_frames ||
        output.size() / channels > max_output_frames ||
        (!input.empty() && input.data() == nullptr) ||
        (!output.empty() && output.data() == nullptr) || overlaps(input, output))
        return {AsrcStatus::invalid};
    if (!std::all_of(input.begin(), input.end(), [](float value) { return std::isfinite(value); }))
        return {AsrcStatus::invalid};
    if (state_->failed)
        return {AsrcStatus::failed};
    if (state_->finished)
        return input.empty() && end_of_input ? AsrcProcessResult{AsrcStatus::finished}
                                            : AsrcProcessResult{AsrcStatus::invalid};
    if (state_->draining && !end_of_input)
        return {AsrcStatus::invalid};
    if (output.empty() || (input.empty() && !end_of_input))
        return {AsrcStatus::no_progress};

    SRC_DATA data{};
    data.data_in = input.empty() ? &state_->empty_input : input.data();
    data.data_out = output.data();
    data.input_frames = static_cast<long>(input.size() / channels);
    data.output_frames = static_cast<long>(output.size() / channels);
    data.src_ratio = ratio_from_ppm(requested_ppm);
    data.end_of_input = end_of_input ? 1 : 0;
    state_->draining = end_of_input;
    const int error = src_process(state_->converter, &data);
    if (error != 0 || data.input_frames_used < 0 || data.output_frames_gen < 0 ||
        data.input_frames_used > data.input_frames || data.output_frames_gen > data.output_frames) {
        state_->failed = true;
        // Do not expose unspecified partial output as consumable media.
        std::fill(output.begin(), output.end(), 0.0F);
        return {AsrcStatus::failed, 0, 0, error};
    }
    const auto used = static_cast<std::size_t>(data.input_frames_used);
    const auto generated = static_cast<std::size_t>(data.output_frames_gen);
    const auto produced = output.first(generated * channels);
    if (!std::all_of(produced.begin(), produced.end(), [](float value) { return std::isfinite(value); })) {
        state_->failed = true;
        std::fill(output.begin(), output.end(), 0.0F);
        return {AsrcStatus::failed};
    }
    if (used != 0 || generated != 0)
        return {AsrcStatus::progress, used, generated};
    if (end_of_input && input.empty()) {
        state_->finished = true;
        return {AsrcStatus::finished};
    }
    return {AsrcStatus::no_progress};
}

bool AsrcStereo::reset(double initial_ppm) noexcept
{
    if (!valid_ppm(initial_ppm))
        return false;
    const int reset_error = src_reset(state_->converter);
    const int ratio_error = reset_error == 0
        ? src_set_ratio(state_->converter, ratio_from_ppm(initial_ppm)) : 0;
    state_->failed = reset_error != 0 || ratio_error != 0;
    state_->draining = false;
    state_->finished = false;
    return !state_->failed;
}

} // namespace avsync
