// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "avsync/audio_correction.hpp"
#include <atomic>
#include <memory>
#include <mutex>

namespace avsync {
// Value-owned association decoded from ONE strictly validated ordered RTP packet.
// Arrival/SR times are health/deadline inputs only, never capture-time substitutes.
struct DiagnosticAudioPacket {
    wire::AudioRecord record;
    std::uint32_t ssrc{}, timestamp{}, frames{};
    Nanoseconds arrival_ns{}, last_report_ns{-1};
    std::array<float,wire::maximum_audio_payload_frames*2> pcm{};
};
[[nodiscard]] bool decode_l24(std::span<const std::byte> payload,
                             DiagnosticAudioPacket& packet) noexcept;
struct DiagnosticAudioSession {
    SessionToken epoch;
    std::uint32_t ssrc{};
    std::uint64_t packets{}, delivered_frames{}, dispatches{}, dispatch_calls_max{};
    std::uint64_t first_frame{}, last_frame{};
    Nanoseconds first_capture_ns{}, last_capture_ns{}, maximum_output_age_ns{};
    std::optional<Nanoseconds> first_fault_ns;
    CorrectionDiagnostics correction;
    CorrectionState state{CorrectionState::priming};
    CorrectionFault fault{CorrectionFault::none};
    double command_ppm{}, target_ppm{}, peak{};
    long double sum_squares{};
};
// Strict finite-fixture verdict: one clean session, or exactly two with the
// first fault caused at the explicitly timed provider pause. Recovery alone
// never hides an additional unplanned interruption.
[[nodiscard]] bool correction_diagnostic_pass(std::span<const DiagnosticAudioSession> sessions,
    bool queue_failed,std::optional<Nanoseconds> provider_pause_ns={}) noexcept;
struct DiagnosticAudioOutput {
    void* context{};
    bool (*consume)(void*,const CorrectedAudio&,std::span<const float>,Nanoseconds) noexcept{};
    void (*revoke)(void*) noexcept{};
};
// Finite desktop-only diagnostic bridge. Defaults to inspect/discard. Explicit
// output hooks run on the ordinary tick owner, NEVER the network/OBS callback.
// An output-enabled instance fails closed on ANY generation fault; no recovery.
// submit(): network callback, short mutex-protected fixed-size copy, no DSP.
// tick(): ordinary single owner, no mutex held during DSP/analysis. At most
// 64 input packets and eight DSP quanta per tick; optional synchronous output
// consumes a borrowed span before it is cleared. Never retain that span.
// snapshot accessors require producer callbacks stopped and final tick complete.
class AudioCorrectionDiagnostic {
public:
    static constexpr std::size_t capacity=64;
    static constexpr Nanoseconds maximum_queue_age_ns=100'000'000;
    explicit AudioCorrectionDiagnostic(std::uint64_t clock_epoch,DiagnosticAudioOutput output={});
    [[nodiscard]] bool submit(const DiagnosticAudioPacket& packet) noexcept;
    void tick(Nanoseconds now,bool provider_healthy);
    [[nodiscard]] bool failed() const noexcept { return failed_.load(); }
    [[nodiscard]] std::size_t queue_peak() const noexcept { return queue_peak_; }
    [[nodiscard]] std::span<const DiagnosticAudioSession> sessions() const noexcept {
        return std::span(sessions_).first(session_count_);
    }
private:
    void snapshot() noexcept;
    void fail(Nanoseconds now) noexcept;
    DiagnosticAudioOutput destination_;
    std::uint64_t clock_epoch_;
    wire::AudioStreamAdmission admission_;
    std::mutex mutex_;
    std::array<DiagnosticAudioPacket,capacity> queue_{};
    std::size_t head_{},size_{},queue_peak_{};
    std::atomic<bool> failed_{};
    std::unique_ptr<AudioCorrectionWorker> worker_;
    std::array<DiagnosticAudioSession,8> sessions_{};
    std::size_t session_count_{};
    std::optional<Nanoseconds> last_tick_;
    Nanoseconds last_report_{-1};
    std::array<float,AudioCorrectionWorker::output_capacity*2> output_{};
};
} // namespace avsync
