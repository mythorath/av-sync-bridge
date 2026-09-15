// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <memory>
#include <span>

namespace avsync {

enum class AsrcStatus { progress, no_progress, finished, invalid, failed };

struct AsrcProcessResult {
    AsrcStatus status = AsrcStatus::no_progress;
    std::size_t input_frames_used = 0;
    std::size_t output_frames_generated = 0;
    // Nonzero only for a libsamplerate error. failed with zero denotes a
    // violated backend count/finite-output contract; discard this generation.
    int library_error = 0;
};

// An optional, single-worker-thread sample converter, not a clock controller.
// No timestamps, gain control, media queues, device access, or OBS callbacks.
// Construction allocates the library's BEST_QUALITY stereo sinc state. The
// wrapper itself never allocates, waits, or performs I/O while processing.
class AsrcStereo {
public:
    static constexpr std::size_t channels = 2;
    static constexpr std::size_t max_input_frames = 9'600;
    static constexpr std::size_t max_output_frames = 3'840;
    static constexpr double max_abs_ppm = 500.0;

    // Initial ratio is set before media, with no startup ramp from unity.
    // Throws invalid_argument for ppm outside the finite +/-500 limit;
    // throws runtime_error (or bad_alloc) when initialization fails.
    explicit AsrcStereo(double initial_ppm = 0.0);
    ~AsrcStereo();
    AsrcStereo(const AsrcStereo&) = delete;
    AsrcStereo& operator=(const AsrcStereo&) = delete;
    AsrcStereo(AsrcStereo&&) = delete;
    AsrcStereo& operator=(AsrcStereo&&) = delete;

    // Input/output are disjoint interleaved stereo arrays. Counts are floats,
    // not frames; malformed, oversized, overlapping or nonfinite input is
    // rejected before touching library state. Finite PCM is not gain-clipped.
    // One call makes at most one bounded src_process call. Advance only by the
    // returned counts and retain unconsumed input. requested_ppm maps to the
    // output/input ratio 1 / (1 + ppm / 1e6), using library smoothing. Output
    // capacity and actual progress affect its ramp; no chunk invariance or
    // caller-independent slew limit is promised.
    //
    // Empty output, or empty input without EOS, returns no_progress and does
    // not update the ratio. An EOS call starts bounded segment draining:
    // continue supplying only unconsumed final input with end_of_input=true,
    // then empty EOS calls until finished. Non-EOS calls after draining starts
    // are invalid. A failed state or completed segment requires reset before
    // more media. Do not drain on clock failure, privacy cutoff, or restart.
    [[nodiscard]] AsrcProcessResult process(
        std::span<const float> input_interleaved,
        std::span<float> output_interleaved,
        double requested_ppm,
        bool end_of_input = false) noexcept;

    // Discard filter history and EOS/failure state, then set a fresh initial
    // ratio. An invalid ppm is rejected without changing current state.
    // Reset the caller-owned sample/timestamp ledger separately.
    [[nodiscard]] bool reset(double initial_ppm = 0.0) noexcept;

    [[nodiscard]] static bool valid_ppm(double ppm) noexcept;
    [[nodiscard]] static const char* backend_version() noexcept;

private:
    struct State;
    std::unique_ptr<State> state_;
};

} // namespace avsync
