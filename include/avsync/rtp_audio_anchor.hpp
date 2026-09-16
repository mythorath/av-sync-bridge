// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "avsync/audio_wire.hpp"
#include <gst/gst.h>
#include <array>
#include <cstdint>
#include <optional>

namespace avsync::net {

inline constexpr unsigned audio_anchor_extension_id = 1;
inline constexpr std::size_t audio_anchor_rtp_limit = 1200;
inline constexpr std::size_t audio_anchor_preextension_limit = 1096;
inline constexpr std::size_t pinned_audio_rtcp_limit = 1500;

// Narrow experimental RTP profile: V2/PT96 stereo L24, 1..180 frames,
// no CSRC/padding or pre-existing extension. Adds exactly one RFC8285 two-byte
// element, ID1/appbits0, with a 96-byte original-anchor record. Consumes the
// caller's existing buffer reference only through gst_buffer_make_writable;
// always retain/use the updated pointer. On false, drop the packet: an insertion
// failure may already have modified it. No exceptions escape these functions.
// This is worker/probe code, not a no-allocation audio callback API.
[[nodiscard]] bool add_audio_anchor(GstBuffer*& buffer,
                                   const wire::AudioRecord& record) noexcept;

// Strictly accepts only the above profile, including exact zero extension pad.
// Rejects unknown/duplicate elements, incorrect framing, absent metadata and
// invalid codec records. Header outputs are zero on failure. It does not admit
// sessions, verify timestamp/frame continuity, or authenticate a peer; pass the
// returned record AND this packet's PCM identity to AudioReceiverValidator.
[[nodiscard]] std::optional<wire::AudioRecord> read_audio_anchor(
    GstBuffer* buffer, std::uint32_t& ssrc, std::uint32_t& timestamp,
    std::uint32_t& frames) noexcept;

enum class PinnedIngressResult { accepted, foreign_identity, malformed, capacity_exhausted };

// Pre-jitter identity fence for an explicitly agreed provider/sender process
// pair. This is not ordered media admission, a freshness check or authentication.
// Foreign well-formed RTP never consumes an SSRC slot. Caller serializes access;
// storage remains bounded by the existing eight-generation diagnostic budget.
class PinnedAudioIngress {
public:
    PinnedAudioIngress(std::uint64_t expected_clock_epoch, std::uint64_t expected_sender_session);
    [[nodiscard]] PinnedIngressResult observe_rtp(GstBuffer*) noexcept;
    [[nodiscard]] bool known_ssrc(std::uint32_t) const noexcept;
    // Permit only SR/SDES compounds whose every source has appeared in strict,
    // identity-matching RTP. Early reports can be retried at the next SR. Other
    // packet types are outside this narrow sender diagnostic profile.
    [[nodiscard]] bool allows_rtcp(GstBuffer*) const noexcept;
    [[nodiscard]] std::size_t ssrc_count() const noexcept { return count_; }
private:
    std::uint64_t clock_epoch_{}, sender_session_{};
    std::array<std::uint32_t, wire::AudioStreamAdmission::maximum_ssrcs> ssrcs_{};
    std::size_t count_{};
};

} // namespace avsync::net
