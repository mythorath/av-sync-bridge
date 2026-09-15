// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <gst/audio/audio.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace avsync::audio {

enum class NominalAudioStatus { progress, no_progress, finished, invalid, failed };

struct NominalAudioResult {
    NominalAudioStatus status{NominalAudioStatus::invalid};
    std::size_t input_frames_used{};
    std::size_t output_frames_generated{};
};

// Stateful, timestamp-free nominal-rate conversion, NOT drift correction. The
// original device sample position and capture clock must travel separately.
// All methods belong to one worker thread; this is not an audio callback API.
class NominalAudioConverter {
public:
    static constexpr std::size_t channels = 2;
    static constexpr std::uint32_t output_rate = 48'000;
    using Mix = std::array<std::array<float, 8>, channels>;

    // Interleaved full-width PCM: S8/U8 or native-endian S16/S24/S32/F32/F64;
    // 1..8 channels, 8..384 kHz with 48000/gcd(input_rate,48000) <= 640.
    // This includes standard audio rates and bounds exact-phase filter tables;
    // pathological coprime rates are rejected. max_input_frames is in
    // 1..input_rate/2. Coefficients use full directly calculated filter phases,
    // not the backend's default interpolated coefficient approximation.
    // Matrix coefficients are finite [-1,1]; unused columns must be zero.
    // Throws std::invalid_argument or std::runtime_error on setup failure.
    NominalAudioConverter(const GstAudioInfo& input, const Mix& mix,
                          std::size_t max_input_frames);
    ~NominalAudioConverter();
    NominalAudioConverter(const NominalAudioConverter&) = delete;
    NominalAudioConverter& operator=(const NominalAudioConverter&) = delete;
    NominalAudioConverter(NominalAudioConverter&&) = delete;
    NominalAudioConverter& operator=(NominalAudioConverter&&) = delete;

    // Exact output for the next call in CURRENT state, not a stateless ratio.
    // Throws if the requested input count is outside the constructor bound.
    [[nodiscard]] std::size_t required_output_frames(std::size_t input_frames) const;
    [[nodiscard]] std::size_t max_latency_input_frames() const noexcept;
    [[nodiscard]] std::size_t max_input_frames() const noexcept;

    // Consumes all input on success, including initial filter lookahead calls
    // producing zero output. A zero-input call does nothing. The first output
    // frame is on the nominal input-origin grid; filter lookahead delays its
    // availability, not its content timestamp. No per-packet phase resets.
    // Input span must contain exactly input_frames*bpf bytes; output may be
    // larger than required. Input/output must not overlap. Invalid input,
    // non-finite float PCM, and insufficient output leave state untouched.
    // A backend failure latches failed until reset and clears produced output.
    [[nodiscard]] NominalAudioResult process(std::span<const std::byte> input,
                                             std::size_t input_frames,
                                             std::span<float> output) noexcept;

    // Explicit offline/clean-EOS operation. Supplies zero lookahead internally
    // and emits ONLY the remaining frames up to floor(real_input*48000/rate).
    // Padding is never counted as captured input. No fabricated live silence:
    // do not call finish() on packet loss, clock fault, mute, or discontinuity;
    // discard/reset that generation instead. One successful call ends input.
    [[nodiscard]] std::size_t required_finish_output_frames() const noexcept;
    [[nodiscard]] NominalAudioResult finish(std::span<float> output) noexcept;
    // Reconstructs the backend to reset fractional phase as well as history.
    // Setup/recovery only: may allocate. A setup failure latches failed.
    [[nodiscard]] bool reset() noexcept;

    [[nodiscard]] std::uint64_t input_frames_processed() const noexcept;
    [[nodiscard]] std::uint64_t output_frames_processed() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace avsync::audio
