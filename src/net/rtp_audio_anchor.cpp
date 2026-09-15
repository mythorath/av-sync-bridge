// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/rtp_audio_anchor.hpp"
#include <gst/rtp/gstrtpbuffer.h>
#include <span>

namespace avsync::net {
namespace {
constexpr std::size_t header_size = 12;
constexpr std::size_t extension_size = 104;
constexpr std::size_t payload_offset = header_size + extension_size;
struct ReadMap {
    GstBuffer* buffer{};
    GstMapInfo info = GST_MAP_INFO_INIT;
    explicit ReadMap(GstBuffer* value) noexcept {
        if (value && gst_buffer_map(value, &info, GST_MAP_READ)) buffer = value;
    }
    ~ReadMap() { if (buffer) gst_buffer_unmap(buffer, &info); }
};
std::uint32_t u32(const guint8* p) noexcept {
    return (static_cast<std::uint32_t>(p[0]) << 24) |
           (static_cast<std::uint32_t>(p[1]) << 16) |
           (static_cast<std::uint32_t>(p[2]) << 8) | p[3];
}
bool valid_payload(std::size_t bytes) noexcept {
    return bytes >= 6 && bytes % 6 == 0 && bytes / 6 <= wire::maximum_audio_payload_frames;
}
bool plain_profile(GstBuffer* buffer) noexcept {
    if (!buffer || gst_buffer_get_size(buffer) > audio_anchor_preextension_limit) return false;
    ReadMap map(buffer);
    return map.buffer && map.info.size >= header_size && map.info.data[0] == 0x80 &&
        (map.info.data[1] & 0x7f) == 96 && valid_payload(map.info.size - header_size);
}
} // namespace

std::optional<wire::AudioRecord> read_audio_anchor(GstBuffer* buffer,
    std::uint32_t& ssrc, std::uint32_t& timestamp, std::uint32_t& frames) noexcept {
    ssrc = timestamp = frames = 0;
    if (!buffer || gst_buffer_get_size(buffer) > audio_anchor_rtp_limit) return {};
    ReadMap map(buffer);
    if (!map.buffer || map.info.size < payload_offset || map.info.data[0] != 0x90 ||
        (map.info.data[1] & 0x7f) != 96 || !valid_payload(map.info.size - payload_offset)) return {};
    const auto* p = map.info.data;
    // Extension type 0x1000 (two-byte header/appbits0), exactly 25 words:
    // ID1, length96, record96, two required zero pad bytes. Exact framing also
    // rejects duplicate or unrelated elements rather than trusting nth=0.
    if (p[12] != 0x10 || p[13] != 0 || p[14] != 0 || p[15] != 25 ||
        p[16] != audio_anchor_extension_id || p[17] != wire::audio_record_size ||
        p[114] != 0 || p[115] != 0) return {};
    const auto decoded = wire::decode(std::as_bytes(std::span(p + 18, wire::audio_record_size)));
    if (!decoded) return {};
    timestamp = u32(p + 4); ssrc = u32(p + 8);
    frames = static_cast<std::uint32_t>((map.info.size - payload_offset) / 6);
    return decoded;
}

bool add_audio_anchor(GstBuffer*& buffer, const wire::AudioRecord& record) noexcept {
    const auto encoded = wire::encode(record);
    if (!encoded || !plain_profile(buffer)) return false;
    // Never append to an existing extension and never expand an unbounded RTP
    // packet. GStreamer can copy a referenced buffer, so propagate ownership.
    buffer = gst_buffer_make_writable(buffer);
    if (!buffer) return false;
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READWRITE, &rtp)) return false;
    const bool inserted = gst_rtp_buffer_add_extension_twobytes_header(&rtp, 0,
        audio_anchor_extension_id, encoded->data(), static_cast<guint>(encoded->size()));
    gst_rtp_buffer_unmap(&rtp);
    if (!inserted || gst_buffer_get_size(buffer) > audio_anchor_rtp_limit) return false;
    std::uint32_t ssrc{}, timestamp{}, frames{};
    const auto decoded = read_audio_anchor(buffer, ssrc, timestamp, frames);
    return decoded && *decoded == record;
}

} // namespace avsync::net
