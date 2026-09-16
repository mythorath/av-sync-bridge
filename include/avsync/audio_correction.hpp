// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "avsync/asrc.hpp"
#include "avsync/audio_wire.hpp"
#include "avsync/audio_phase.hpp"
#include <array>
#include <span>

namespace avsync {
enum class CorrectionState { priming, running, faulted };
enum class CorrectionFault { none, metadata, pcm, rate, overflow, stale, health, backend, timeline, phase };
enum class CorrectionStaleReason { none, no_progress, anchor_age, input_queue, output_queue };
enum class CorrectionPush { accepted, priming_discard, wrong_epoch, rejected };
struct CorrectionDiagnostics {
    std::uint64_t received_frames{}, priming_discarded_frames{}, consumed_frames{}, produced_frames{}, delivered_frames{};
    std::uint64_t estimator_windows{}, backend_calls{}, no_progress_dispatches{};
    std::size_t peak_input_frames{}, peak_output_frames{};
    double maximum_command_step_ppm{};
    double acquisition_spread_ppm{};
    std::uint64_t phase_checks{}, phase_waits{};
    std::size_t peak_phase_anchors{};
    Nanoseconds maximum_predicted_phase_ns{};
    // First stale gate, captured before fail() erases private DSP/queue state.
    // Missing age means checked subtraction failed, not an age of zero.
    CorrectionStaleReason stale_reason{CorrectionStaleReason::none};
    std::optional<Nanoseconds> stale_age_ns;
};
struct CorrectedAudio {
    SessionToken epoch;
    std::uint64_t first_frame{};
    Nanoseconds capture_grid_ns{};
    std::size_t frames{};
};

// Single-owner, explicitly experimental worker; no thread, socket, device or
// OBS access. Caller supplies already ordered PCM and its ORIGINAL wire record.
// Float PCM must be decoded from that same validated packet by the caller.
// Worker-owned storage is fixed after construction; all queued PCM is value-owned.
// Library allocation behavior still requires tracing on each supported build.
class AudioCorrectionWorker {
public:
    static constexpr std::size_t input_capacity = 9'600, output_capacity = 3'840;
    static constexpr std::size_t quantum = 480, input_offer = 2'048, max_calls = 8;
    // Fixed full-output calls avoid partial-call acceleration. A conservative
    // 99 ppm/s command leaves margin for the audited sinc ramp's endpoint lag
    // and reciprocal ratio transformation; this is not an API-wide guarantee.
    static constexpr double command_step_ppm = 0.99;
    static constexpr Nanoseconds queue_lifetime_ns = 200'000'000;
    AudioCorrectionWorker(SessionToken epoch, std::uint64_t clock_epoch);
    AudioCorrectionWorker(const AudioCorrectionWorker&) = delete;
    AudioCorrectionWorker& operator=(const AudioCorrectionWorker&) = delete;

    // upstream_healthy is an explicit caller gate, not inferred from arrival.
    // Wrong epochs leave active state alone; other invalid input fails closed.
    [[nodiscard]] CorrectionPush push(const wire::AudioRecord&, std::uint32_t ssrc,
        std::uint32_t rtp_timestamp, std::span<const float> stereo, Nanoseconds now,
        bool upstream_healthy) noexcept;
    // At most eight fixed 480-frame src_process calls. Insufficient input or
    // output room makes no progress and never advances the rate command.
    // A partial backend output is a visible fault, never quietly re-ramped.
    [[nodiscard]] std::size_t dispatch(Nanoseconds now, bool upstream_healthy) noexcept;
    // Arbitrary bounded output partitions do not change DSP block boundaries.
    [[nodiscard]] std::optional<CorrectedAudio> pull(std::span<float> stereo,
        Nanoseconds now, bool upstream_healthy) noexcept;
    // Discards filter, queues and counters, never drains an old generation.
    // Caller owns global transport admission and must authorize the successor.
    [[nodiscard]] bool reset(SessionToken next) noexcept;
    [[nodiscard]] CorrectionState state() const noexcept { return state_; }
    [[nodiscard]] CorrectionFault fault() const noexcept { return fault_; }
    [[nodiscard]] const CorrectionDiagnostics& diagnostics() const noexcept { return diagnostics_; }
    [[nodiscard]] std::size_t pending_input() const noexcept { return input_size_; }
    [[nodiscard]] std::size_t pending_output() const noexcept { return output_size_; }
    [[nodiscard]] double command_ppm() const noexcept { return command_; }
    [[nodiscard]] double target_ppm() const noexcept { return target_; }
    [[nodiscard]] std::optional<Nanoseconds> origin_ns() const noexcept { return origin_; }
    [[nodiscard]] std::uint64_t origin_wire_frame() const noexcept { return origin_wire_; }

private:
    bool check_health(Nanoseconds now, bool healthy) noexcept;
    void fail(CorrectionFault reason) noexcept;
    bool start(const wire::AudioRecord&) noexcept;
    SessionToken epoch_;
    std::uint64_t clock_epoch_;
    wire::AudioReceiverValidator validator_;
    AsrcStereo backend_;
    SincPhaseModel phase_model_;
    AudioPhaseLedger phase_ledger_;
    std::array<PredictedSourcePosition, quantum> phase_positions_{};
    CorrectionState state_{CorrectionState::priming};
    CorrectionFault fault_{CorrectionFault::none};
    CorrectionDiagnostics diagnostics_;
    std::array<double,3> acquisition_{};
    std::size_t acquisition_count_{};
    double target_{}, command_{};
    std::optional<Nanoseconds> origin_, last_now_, last_progress_;
    std::optional<RationalTimeline> timeline_;
    std::uint64_t origin_wire_{}, output_index_{};
    std::array<float, input_capacity * 2> input_{};
    std::array<Nanoseconds, input_capacity> input_arrivals_{};
    std::size_t input_head_{}, input_size_{};
    std::array<float, output_capacity * 2> output_{};
    std::array<Nanoseconds, output_capacity> output_arrivals_{};
    std::size_t output_head_{}, output_size_{};
    std::array<float, input_offer * 2> scratch_input_{};
    std::array<float, quantum * 2> scratch_output_{};
};
} // namespace avsync
