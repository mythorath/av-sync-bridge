// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/nominal_audio.hpp"

#include <gst/audio/audio-converter.h>
#include <gst/audio/audio-resampler.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <vector>

namespace avsync::audio {
namespace {
bool supported_format(GstAudioFormat format) noexcept
{
    switch (format) {
    case GST_AUDIO_FORMAT_S8:
    case GST_AUDIO_FORMAT_U8:
    case GST_AUDIO_FORMAT_S16:
    case GST_AUDIO_FORMAT_S24:
    case GST_AUDIO_FORMAT_S32:
    case GST_AUDIO_FORMAT_F32:
    case GST_AUDIO_FORMAT_F64: return true;
    default: return false;
    }
}

bool overlap(std::span<const std::byte> input, std::span<float> output) noexcept
{
    if (output.size() > std::numeric_limits<std::size_t>::max() / sizeof(float)) return true;
    if (input.empty() || output.empty()) return false;
    const auto in = reinterpret_cast<std::uintptr_t>(input.data());
    const auto out = reinterpret_cast<std::uintptr_t>(output.data());
    if (input.size() > std::numeric_limits<std::uintptr_t>::max() - in ||
        output.size_bytes() > std::numeric_limits<std::uintptr_t>::max() - out)
        return true;
    return in < out + output.size_bytes() && out < in + input.size();
}

template<class T> bool finite_pcm(std::span<const std::byte> input) noexcept
{
    for (std::size_t offset = 0; offset < input.size(); offset += sizeof(T)) {
        T value;
        std::memcpy(&value, input.data() + offset, sizeof(value));
        if (!std::isfinite(value)) return false;
    }
    return true;
}

std::uint64_t duration_frames(std::uint64_t input_frames, std::uint32_t rate) noexcept
{
    return (input_frames / rate) * NominalAudioConverter::output_rate +
           ((input_frames % rate) * NominalAudioConverter::output_rate) / rate;
}
} // namespace

struct NominalAudioConverter::Impl {
    GstAudioConverter* converter{};
    GstAudioInfo source{}, target{};
    GstStructure* config{};
    std::size_t maximum_input{}, bytes_per_frame{}, latency{}, maximum_finish_input{};
    std::uint32_t rate{};
    GstAudioFormat format{};
    std::uint64_t input_total{}, output_total{};
    bool finished{}, failed{};
    float output_dummy[2]{};
    std::vector<std::byte> finish_padding;
    ~Impl()
    {
        if (converter) gst_audio_converter_free(converter);
        if (config) gst_structure_free(config);
    }

    bool valid_output(std::span<float> output, std::size_t frames) noexcept
    {
        const auto used = output.first(frames * 2);
        if (std::all_of(used.begin(), used.end(), [](float x) { return std::isfinite(x); }))
            return true;
        std::fill(used.begin(), used.end(), 0.0F);
        failed = true;
        return false;
    }
};

NominalAudioConverter::NominalAudioConverter(const GstAudioInfo& input, const Mix& mix,
                                             std::size_t max_input_frames)
    : impl_(std::make_unique<Impl>())
{
    if (!input.finfo || !supported_format(input.finfo->format) ||
        input.finfo != gst_audio_format_get_info(input.finfo->format) ||
        input.layout != GST_AUDIO_LAYOUT_INTERLEAVED || input.channels < 1 ||
        input.channels > 8 || input.rate < 8'000 || input.rate > 384'000 ||
        input.bpf != input.channels * input.finfo->width / 8 ||
        input.finfo->depth != input.finfo->width || max_input_frames == 0 ||
        max_input_frames > static_cast<std::size_t>(input.rate / 2) ||
        output_rate / std::gcd(output_rate, static_cast<std::uint32_t>(input.rate)) > 640)
        throw std::invalid_argument("unsupported nominal audio format or frame bound");
    for (const auto& row : mix) {
        for (std::size_t c = 0; c < row.size(); ++c) {
            if (!std::isfinite(row[c]) || std::abs(row[c]) > 1.0F ||
                (c >= static_cast<std::size_t>(input.channels) && row[c] != 0.0F))
                throw std::invalid_argument("invalid nominal audio mix matrix");
        }
    }

    auto* config = gst_structure_new_empty("nominal-audio-converter");
    gst_structure_set(config,
        GST_AUDIO_CONVERTER_OPT_RESAMPLER_METHOD, GST_TYPE_AUDIO_RESAMPLER_METHOD,
        GST_AUDIO_RESAMPLER_METHOD_KAISER,
        GST_AUDIO_RESAMPLER_OPT_FILTER_MODE, GST_TYPE_AUDIO_RESAMPLER_FILTER_MODE,
        GST_AUDIO_RESAMPLER_FILTER_MODE_FULL,
        GST_AUDIO_RESAMPLER_OPT_FILTER_INTERPOLATION, GST_TYPE_AUDIO_RESAMPLER_FILTER_INTERPOLATION,
        GST_AUDIO_RESAMPLER_FILTER_INTERPOLATION_NONE, nullptr);
    gst_audio_resampler_options_set_quality(GST_AUDIO_RESAMPLER_METHOD_KAISER, 10,
                                             input.rate, output_rate, config);
    GValue matrix = G_VALUE_INIT;
    g_value_init(&matrix, GST_TYPE_ARRAY);
    for (const auto& row : mix) {
        GValue values = G_VALUE_INIT;
        g_value_init(&values, GST_TYPE_ARRAY);
        for (int c = 0; c < input.channels; ++c) {
            GValue value = G_VALUE_INIT;
            g_value_init(&value, G_TYPE_FLOAT);
            g_value_set_float(&value, row[static_cast<std::size_t>(c)]);
            gst_value_array_append_value(&values, &value);
            g_value_unset(&value);
        }
        gst_value_array_append_value(&matrix, &values);
        g_value_unset(&values);
    }
    gst_structure_set_value(config, GST_AUDIO_CONVERTER_OPT_MIX_MATRIX, &matrix);
    g_value_unset(&matrix);
    auto source = input;
    GstAudioInfo target;
    gst_audio_info_init(&target);
    gst_audio_info_set_format(&target, GST_AUDIO_FORMAT_F32, output_rate, 2, nullptr);
    impl_->source = source;
    impl_->target = target;
    impl_->config = gst_structure_copy(config);
    impl_->converter = gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE,
                                                &source, &target, config);
    // gst_audio_converter_new takes ownership of config, including failure.
    if (!impl_->converter) throw std::runtime_error("nominal audio converter setup failed");
    impl_->maximum_input = max_input_frames;
    impl_->bytes_per_frame = static_cast<std::size_t>(input.bpf);
    impl_->rate = static_cast<std::uint32_t>(input.rate);
    impl_->format = input.finfo->format;
    impl_->latency = gst_audio_converter_get_max_latency(impl_->converter);
    // Quality 10 at the accepted <=384 kHz rates needs at most 2048 taps in
    // supported backends. With <=640 phases and <=8-byte coefficients, even
    // this conservative 4096-tap safety cap bounds coefficient payload to
    // 20 MiB (other backend allocations are separate, bounded by input size).
    if (impl_->latency > 2048 || impl_->latency > static_cast<std::size_t>(input.rate / 2))
        throw std::runtime_error("nominal converter filter latency exceeds bound");
    impl_->maximum_finish_input = impl_->latency +
        (impl_->rate + output_rate - 1) / output_rate + 2;
    impl_->finish_padding.resize(impl_->maximum_finish_input * impl_->bytes_per_frame,
        impl_->format == GST_AUDIO_FORMAT_U8 ? std::byte{0x80} : std::byte{0});

}

NominalAudioConverter::~NominalAudioConverter() = default;

std::size_t NominalAudioConverter::required_output_frames(std::size_t input_frames) const
{
    if (input_frames > impl_->maximum_input)
        throw std::invalid_argument("nominal input frame bound exceeded");
    if (impl_->failed || impl_->finished)
        throw std::logic_error("nominal converter is not accepting input");
    return input_frames ? gst_audio_converter_get_out_frames(impl_->converter, input_frames) : 0;
}

std::size_t NominalAudioConverter::max_latency_input_frames() const noexcept { return impl_->latency; }
std::size_t NominalAudioConverter::max_input_frames() const noexcept { return impl_->maximum_input; }

NominalAudioResult NominalAudioConverter::process(std::span<const std::byte> input,
                                                 std::size_t input_frames,
                                                 std::span<float> output) noexcept
{
    using S = NominalAudioStatus;
    if (impl_->failed) return {S::failed};
    if (impl_->finished) return {input.empty() && !input_frames ? S::finished : S::invalid};
    if (input_frames > impl_->maximum_input ||
        input.size() != input_frames * impl_->bytes_per_frame || overlap(input, output))
        return {S::invalid};
    if (!input_frames) return {S::no_progress};
    // Keep the duration multiplication provably representable even at 8 kHz.
    constexpr auto maximum_total = std::numeric_limits<std::uint64_t>::max() / output_rate;
    if (input_frames > maximum_total - impl_->input_total) return {S::invalid};
    const auto count = gst_audio_converter_get_out_frames(impl_->converter, input_frames);
    if (count > output.size() / 2 ||
        (impl_->format == GST_AUDIO_FORMAT_F32 && !finite_pcm<float>(input)) ||
        (impl_->format == GST_AUDIO_FORMAT_F64 && !finite_pcm<double>(input)))
        return {S::invalid};
    gpointer in[] = {const_cast<std::byte*>(input.data())};
    gpointer out[] = {count ? output.data() : impl_->output_dummy};
    if (!gst_audio_converter_samples(impl_->converter, GST_AUDIO_CONVERTER_FLAG_NONE,
                                     in, input_frames, out, count)) {
        std::fill_n(output.begin(), count * 2, 0.0F);
        impl_->failed = true;
        return {S::failed};
    }
    if (!impl_->valid_output(output, count)) return {S::failed};
    impl_->input_total += input_frames;
    impl_->output_total += count;
    return {S::progress, input_frames, count};
}

std::size_t NominalAudioConverter::required_finish_output_frames() const noexcept
{
    if (impl_->failed || impl_->finished) return 0;
    const auto target = duration_frames(impl_->input_total, impl_->rate);
    return target >= impl_->output_total ? static_cast<std::size_t>(target - impl_->output_total) : 0;
}

NominalAudioResult NominalAudioConverter::finish(std::span<float> output) noexcept
{
    using S = NominalAudioStatus;
    if (impl_->failed) return {S::failed};
    if (impl_->finished) return {S::finished};
    const auto target = duration_frames(impl_->input_total, impl_->rate);
    if (target < impl_->output_total) { impl_->failed = true; return {S::failed}; }
    const auto count = required_finish_output_frames();
    if (count > output.size() / 2) return {S::invalid};
    if (count) {
        // get_in_frames() is a ratio/phase helper, not a complete inverse of
        // get_out_frames() during initial filter lookahead. Find the minimal
        // positive padding within the explicit filter bound instead.
        auto padding = impl_->maximum_finish_input;
        if (gst_audio_converter_get_out_frames(impl_->converter, padding) < count) {
            impl_->failed = true;
            return {S::failed};
        }
        std::size_t low = 1;
        while (low < padding) {
            const auto middle = low + (padding - low) / 2;
            if (gst_audio_converter_get_out_frames(impl_->converter, middle) >= count)
                padding = middle;
            else low = middle + 1;
        }
        // Use actual initialized PCM instead of the backend's nullable-input
        // silence shortcut. Its unpack path in supported versions passes a
        // sample count to a byte-length silence helper for wider PCM formats.
        gpointer in[] = {impl_->finish_padding.data()};
        gpointer out[] = {output.data()};
        if (!gst_audio_converter_samples(impl_->converter, GST_AUDIO_CONVERTER_FLAG_NONE,
                                         in, padding, out, count)) {
            std::fill_n(output.begin(), count * 2, 0.0F);
            impl_->failed = true;
            return {S::failed};
        }
        if (!impl_->valid_output(output, count)) return {S::failed};
        impl_->output_total += count;
    }
    impl_->finished = true;
    return {S::finished, 0, count};
}

bool NominalAudioConverter::reset() noexcept
{
    // gst_audio_resampler_reset() clears history but retains fractional phase
    // on supported releases. A new converter is necessary for a new origin.
    // https://gitlab.freedesktop.org/gstreamer/gstreamer/-/blob/1.24.2/subprojects/gst-plugins-base/gst-libs/gst/audio/audio-resampler.c
    auto* fresh = gst_audio_converter_new(GST_AUDIO_CONVERTER_FLAG_NONE,
        &impl_->source, &impl_->target, gst_structure_copy(impl_->config));
    if (!fresh) { impl_->failed = true; return false; }
    gst_audio_converter_free(impl_->converter);
    impl_->converter = fresh;
    impl_->input_total = impl_->output_total = 0;
    impl_->finished = impl_->failed = false;
    return true;
}

std::uint64_t NominalAudioConverter::input_frames_processed() const noexcept { return impl_->input_total; }
std::uint64_t NominalAudioConverter::output_frames_processed() const noexcept { return impl_->output_total; }

} // namespace avsync::audio
