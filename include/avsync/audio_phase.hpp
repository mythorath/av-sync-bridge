// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "avsync/timing.hpp"
#include <array>
#include <span>
#include <string_view>

namespace avsync {
// A model prediction, NOT a position read from libsamplerate's private state.
struct PredictedSourcePosition { std::uint64_t whole{}; double fraction{}; };

// Audited 0.2.2 stereo sinc recurrence, full non-EOS 480-frame calls only.
// Independent original code implementing the mathematical timing model; no
// filter implementation or private library layout is copied/accessed.
class SincPhaseModel {
public:
    static constexpr std::size_t quantum=480;
    static constexpr std::uint64_t maximum_frames=48'000ULL*60*60*24;
    static bool supports(std::string_view version) noexcept;
    bool reset(std::uint64_t wire_origin, double initial_ppm) noexcept;
    // Transactional: invalid arguments/horizon/overflow leave state unchanged.
    bool advance(double requested_ppm, std::span<PredictedSourcePosition> positions) noexcept;
    PredictedSourcePosition next() const noexcept { return next_; }
    double reached_ppm() const noexcept { return (1/ratio_-1)*1e6; }
    std::uint64_t frames() const noexcept { return frames_; }
private:
    PredictedSourcePosition next_{};
    double ratio_{1};
    std::uint64_t frames_{};
    bool initialized_{};
};

enum class PhaseStatus { ready, waiting, invalid, capacity, gap, mismatch };
struct PhaseAssessment {
    PhaseStatus status{PhaseStatus::waiting};
    Nanoseconds maximum_absolute_ns{};
    Nanoseconds first_ns{}, last_ns{}; // Predicted source time minus output grid.
};

// Original-anchor interpolation ledger, not an arrival-time/queue servo.
// Caller validates identity/provenance and updates retain_from after each full
// DSP quantum. Missing brackets block output; never extrapolate/fall back.
class AudioPhaseLedger {
public:
    static constexpr std::size_t capacity=512;
    static constexpr Nanoseconds maximum_anchor_gap_ns=100'000'000;
    static constexpr Nanoseconds phase_limit_ns=10'000'000;
    void clear() noexcept { head_=size_=0; }
    PhaseStatus add(PredictedSourcePosition, Nanoseconds, PredictedSourcePosition retain_from) noexcept;
    PhaseAssessment assess(std::span<const PredictedSourcePosition>, Nanoseconds origin,
                           std::uint64_t first_output_frame) const noexcept;
    std::size_t size() const noexcept { return size_; }
private:
    struct Anchor { PredictedSourcePosition source; Nanoseconds time; };
    const Anchor& at(std::size_t index) const noexcept { return anchors_[(head_+index)%capacity]; }
    std::optional<Nanoseconds> time_at(PredictedSourcePosition) const noexcept;
    std::array<Anchor,capacity> anchors_{};
    std::size_t head_{},size_{};
};
} // namespace avsync
