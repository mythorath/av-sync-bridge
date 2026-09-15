// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/audio_anchors.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace avsync::wire {

inline constexpr std::size_t audio_record_size = 96;
inline constexpr std::uint32_t maximum_audio_payload_frames = 180;

// Original capture provenance and nominal packet position are distinct. No
// native-layout serialization; every integer has an explicit wire offset.
struct AudioRecord {
    SessionToken epoch;
    std::uint64_t clock_epoch{};
    std::uint64_t device_origin{};
    std::uint32_t source_rate{};
    std::uint32_t rtp_zero{};
    std::uint64_t packet_wire_start{};
    std::uint64_t anchor_sequence{};
    std::uint64_t device_position{};
    Nanoseconds capture_ns{};
    std::uint64_t qpc_100ns{};
    std::uint64_t calibration_revision{};
    friend bool operator==(const AudioRecord&, const AudioRecord&) = default;
};

using EncodedAudioRecord = std::array<std::byte, audio_record_size>;
[[nodiscard]] std::optional<EncodedAudioRecord> encode(const AudioRecord&) noexcept;
[[nodiscard]] std::optional<AudioRecord> decode(std::span<const std::byte>) noexcept;

enum class AudioWireStatus {
    accepted, repeated, measured, malformed, wrong_clock, wrong_epoch,
    wrong_ssrc, changed_format, bad_start, bad_timestamp, frame_gap,
    anchor_gap, conflicting_anchor, anchor_ahead, stale, future, overflow,
    requires_reset
};

struct AudioValidationResult {
    AudioWireStatus status{AudioWireStatus::malformed};
    bool accepted{};
    bool repeated{};
    // A newly measured ORIGINAL-clock estimate only, including out-of-range
    // diagnostic results. Never re-emitted for repeated anchors/short windows.
    // For an approved fresh correction command, use current_estimate() instead.
    std::optional<RateEstimate> estimate;
};

// One explicitly admitted SSRC/generation after jitter-buffer ordering. O(1)
// storage, no allocations, PCM queues, arrival-time rebasing or auto recovery.
// Global admission must prevent old/new SSRCs taking over the active source.
class AudioReceiverValidator {
public:
    explicit AudioReceiverValidator(std::uint64_t expected_clock_epoch);
    [[nodiscard]] AudioValidationResult observe(const AudioRecord&, std::uint32_t ssrc,
        std::uint32_t rtp_timestamp, std::uint32_t payload_frames, Nanoseconds now) noexcept;
    // Call for absent/duplicate/malformed target extension or invalid PCM on
    // this ordered branch; do not simply drop metadata and continue its audio.
    [[nodiscard]] AudioValidationResult reject_missing() noexcept;
    [[nodiscard]] bool faulted() const noexcept { return faulted_; }
    [[nodiscard]] bool admitted() const noexcept { return latest_.has_value(); }
    [[nodiscard]] const std::optional<AudioRecord>& latest_record() const noexcept { return latest_; }
    [[nodiscard]] std::optional<RateEstimate> current_estimate(Nanoseconds now) const noexcept;
    [[nodiscard]] std::uint64_t next_wire_frame() const noexcept { return next_wire_frame_; }

private:
    [[nodiscard]] AudioValidationResult reject(AudioWireStatus, bool poison = true) noexcept;
    std::uint64_t expected_clock_epoch_{};
    std::uint32_t ssrc_{};
    std::uint64_t next_wire_frame_{};
    std::optional<AudioRecord> latest_;
    std::optional<AudioAnchorTracker> tracker_;
    bool faulted_{};
};

// Global diagnostic-run admission, after successful per-branch validation.
// Foreign sender sessions require an explicit new run/control agreement; this
// helper never infers identity from arrival order. External caller serializes it.
class AudioStreamAdmission {
public:
    static constexpr std::size_t maximum_ssrcs = 8;
    explicit AudioStreamAdmission(std::uint64_t expected_clock_epoch);
    [[nodiscard]] bool admit(const AudioRecord&, std::uint32_t ssrc) noexcept;
    [[nodiscard]] std::optional<SessionToken> active_epoch() const noexcept { return active_epoch_; }
    [[nodiscard]] std::optional<std::uint32_t> active_ssrc() const noexcept
    { return active_epoch_ ? std::optional<std::uint32_t>(active_ssrc_) : std::nullopt; }
    [[nodiscard]] std::size_t admitted_ssrc_count() const noexcept { return count_; }
private:
    std::uint64_t expected_clock_epoch_{};
    std::optional<SessionToken> active_epoch_;
    std::uint32_t active_ssrc_{};
    std::array<std::uint32_t, maximum_ssrcs> history_{};
    std::size_t count_{};
};

} // namespace avsync::wire
