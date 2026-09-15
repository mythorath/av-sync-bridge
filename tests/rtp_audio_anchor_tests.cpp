// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded generated PCM/packet tests only. No sockets, devices or media files.
#include "avsync/rtp_audio_anchor.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {
using avsync::wire::AudioRecord;
using avsync::wire::AudioReceiverValidator;
using avsync::wire::AudioValidationResult;
constexpr std::uint64_t clock_epoch = 12345;
constexpr std::uint32_t rtp_zero = 0xffffff00U, ssrc_value = 0x31415926U;
constexpr avsync::Nanoseconds origin_ns = 10'000'000'000;
unsigned checks{};
void check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
struct BufferDelete { void operator()(GstBuffer* value) const { if (value) gst_buffer_unref(value); } };
using Buffer = std::unique_ptr<GstBuffer, BufferDelete>;
struct ListDelete { void operator()(GstBufferList* value) const { if (value) gst_buffer_list_unref(value); } };
using List = std::unique_ptr<GstBufferList, ListDelete>;
struct PipelineDelete {
    void operator()(GstElement* value) const {
        if (value) { gst_element_set_state(value, GST_STATE_NULL); gst_object_unref(value); }
    }
};
using Pipeline = std::unique_ptr<GstElement, PipelineDelete>;
struct ElementDelete { void operator()(GstElement* value) const { if (value) gst_object_unref(value); } };
using Element = std::unique_ptr<GstElement, ElementDelete>;
struct PadDelete { void operator()(GstPad* value) const { if (value) gst_object_unref(value); } };
using Pad = std::unique_ptr<GstPad, PadDelete>;

std::uint32_t identity(std::uint64_t frame, bool right = false) {
    const auto value = static_cast<std::uint32_t>(frame * 17 + 19);
    return right ? ((~value + 1) & 0xffffffU) : value;
}
void put24(guint8* p, std::uint32_t value) {
    p[0] = static_cast<guint8>(value >> 16); p[1] = static_cast<guint8>(value >> 8);
    p[2] = static_cast<guint8>(value);
}
std::uint32_t get24(const guint8* p) {
    return (static_cast<std::uint32_t>(p[0]) << 16) | (static_cast<std::uint32_t>(p[1]) << 8) | p[2];
}
void fill_pcm(guint8* p, std::uint64_t first, std::uint32_t frames) {
    for (std::uint32_t i = 0; i < frames; ++i) {
        put24(p + i * 6, identity(first + i)); put24(p + i * 6 + 3, identity(first + i, true));
    }
}
AudioRecord record_for(std::uint64_t wire, unsigned rate = 44100) {
    AudioRecord result;
    result.epoch = {17, 3}; result.clock_epoch = clock_epoch;
    result.device_origin = 999997; result.source_rate = rate;
    result.rtp_zero = rtp_zero; result.packet_wire_start = wire;
    // Deliberately nonintegral wire positions for 44.1k and 192k. These are
    // selected original sample anchors, not fabricated packet timestamps.
    const std::uint64_t step = rate / 50 + 1;
    std::uint64_t sequence{};
    for (;;) {
        const auto next = avsync::nominal_wire_position(result.device_origin + (sequence + 1) * step,
            result.device_origin, 0, rate);
        CHECK(next);
        if (next->whole > wire || (next->whole == wire && next->remainder != 0)) break;
        ++sequence;
    }
    result.anchor_sequence = sequence;
    result.device_position = result.device_origin + sequence * step;
    result.capture_ns = origin_ns + static_cast<avsync::Nanoseconds>((sequence * step * 1'000'000'000ULL) / rate);
    result.qpc_100ns = static_cast<std::uint64_t>(result.capture_ns / 100);
    result.calibration_revision = 9 + sequence / 3;
    return result;
}
avsync::Nanoseconds arrival(std::uint64_t wire) {
    return origin_ns + static_cast<avsync::Nanoseconds>(wire * 1'000'000'000ULL / 48000) + 20'000'000;
}
Buffer packet(std::uint64_t start, std::uint32_t frames, bool extension = true) {
    auto* value = gst_rtp_buffer_new_allocate(frames * 6, 0, 0);
    CHECK(value);
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    CHECK(gst_rtp_buffer_map(value, GST_MAP_READWRITE, &rtp));
    gst_rtp_buffer_set_payload_type(&rtp, 96);
    gst_rtp_buffer_set_ssrc(&rtp, ssrc_value);
    gst_rtp_buffer_set_timestamp(&rtp, rtp_zero + static_cast<std::uint32_t>(start));
    fill_pcm(static_cast<guint8*>(gst_rtp_buffer_get_payload(&rtp)), start, frames);
    gst_rtp_buffer_unmap(&rtp);
    if (extension) CHECK(avsync::net::add_audio_anchor(value, record_for(start)));
    return Buffer(value);
}
void byte(GstBuffer* buffer, std::size_t offset, guint8 value) {
    CHECK(offset < gst_buffer_get_size(buffer));
    CHECK(gst_buffer_fill(buffer, offset, &value, 1) == 1);
}
std::vector<guint8> bytes(GstBuffer* buffer) {
    std::vector<guint8> result(gst_buffer_get_size(buffer));
    CHECK(gst_buffer_extract(buffer, 0, result.data(), result.size()) == result.size());
    return result;
}
Buffer from_bytes(const std::vector<guint8>& data) {
    return Buffer(gst_rtp_buffer_new_copy_data(data.data(), data.size()));
}
AudioValidationResult deliver(AudioReceiverValidator& receiver, GstBuffer* buffer,
                              avsync::Nanoseconds now) {
    std::uint32_t ssrc{}, timestamp{}, frames{};
    const auto record = avsync::net::read_audio_anchor(buffer, ssrc, timestamp, frames);
    return record ? receiver.observe(*record, ssrc, timestamp, frames, now) : receiver.reject_missing();
}
bool payload_matches(GstBuffer* buffer, const AudioRecord& record) {
    GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
    if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) return false;
    const auto frames = gst_rtp_buffer_get_payload_len(&rtp) / 6;
    const auto* p = static_cast<const guint8*>(gst_rtp_buffer_get_payload(&rtp));
    bool okay = true;
    for (guint i = 0; i < frames; ++i) {
        okay = okay && get24(p + i * 6) == identity(record.packet_wire_start + i) &&
            get24(p + i * 6 + 3) == identity(record.packet_wire_start + i, true);
    }
    gst_rtp_buffer_unmap(&rtp);
    return okay;
}

void adapter_tests() {
    std::uint32_t ssrc = 4, timestamp = 5, frames = 6;
    CHECK(!avsync::net::read_audio_anchor(nullptr, ssrc, timestamp, frames));
    CHECK(ssrc == 0 && timestamp == 0 && frames == 0);
    GstBuffer* empty{};
    CHECK(!avsync::net::add_audio_anchor(empty, record_for(0)));
    for (const auto count : {1U, 179U, 180U}) {
        auto source = packet(0, count, false);
        Buffer held(gst_buffer_ref(source.get()));
        auto* writable = source.release();
        CHECK(avsync::net::add_audio_anchor(writable, record_for(0)));
        source.reset(writable);
        CHECK(source.get() != held.get());
        CHECK(gst_buffer_get_size(source.get()) == 116 + count * 6);
        CHECK(gst_buffer_get_size(held.get()) == 12 + count * 6);
        const auto decoded = avsync::net::read_audio_anchor(source.get(), ssrc, timestamp, frames);
        CHECK(decoded && *decoded == record_for(0));
        CHECK(ssrc == ssrc_value && timestamp == rtp_zero && frames == count);
        CHECK(payload_matches(source.get(), *decoded));
        writable = source.release();
        CHECK(!avsync::net::add_audio_anchor(writable, record_for(0)));
        source.reset(writable);
    }
    for (const auto count : {0U, 181U, 1000U}) {
        auto invalid = packet(0, count, false);
        auto* value = invalid.release();
        CHECK(!avsync::net::add_audio_anchor(value, record_for(0)));
        invalid.reset(value);
    }
    auto valid = packet(0, 7);
    // Every record/header byte below is part of a separately meaningful gate.
    for (const auto [offset, replacement] : std::array<std::pair<std::size_t, guint8>, 15>{
        {{0, 0x50}, {0, 0x91}, {0, 0xb0}, {1, 95}, {12, 0xbe}, {13, 1},
         {15, 24}, {16, 2}, {17, 95}, {18, 2}, {19, 1}, {20, 1}, {62, 1},
         {114, 1}, {115, 1}}}) {
        Buffer bad(gst_buffer_copy_deep(valid.get()));
        byte(bad.get(), offset, replacement);
        ssrc = timestamp = frames = 99;
        CHECK(!avsync::net::read_audio_anchor(bad.get(), ssrc, timestamp, frames));
        CHECK(ssrc == 0 && timestamp == 0 && frames == 0);
    }
    auto truncated = bytes(valid.get()); truncated.pop_back();
    auto bad_size = from_bytes(truncated);
    CHECK(!avsync::net::read_audio_anchor(bad_size.get(), ssrc, timestamp, frames));
    auto oversized = bytes(valid.get()); oversized.resize(1201);
    auto bad_large = from_bytes(oversized);
    CHECK(!avsync::net::read_audio_anchor(bad_large.get(), ssrc, timestamp, frames));
    // A real duplicate element with valid RFC8285 framing must also fail.
    auto duplicate = bytes(valid.get());
    const std::vector<guint8> element(duplicate.begin() + 16, duplicate.begin() + 114);
    duplicate.erase(duplicate.begin() + 114, duplicate.begin() + 116);
    duplicate.insert(duplicate.begin() + 114, element.begin(), element.end());
    duplicate[15] = 49; // two*(2+96) /4, no extension pad required.
    auto bad_duplicate = from_bytes(duplicate);
    CHECK(!avsync::net::read_audio_anchor(bad_duplicate.get(), ssrc, timestamp, frames));
}

std::vector<Buffer> actual_payloader(bool varied) {
    GError* error{};
    Pipeline pipeline(gst_parse_launch(
        "appsrc name=source format=time is-live=false block=false max-bytes=65536 "
        "caps=audio/x-raw,format=S24BE,layout=interleaved,rate=48000,channels=2 "
        "! rtpL24pay name=pay pt=96 mtu=1096 min-ptime=2000000 max-ptime=3750000 "
        "timestamp-offset=4294967040 seqnum-offset=65530 ssrc=826366246 perfect-rtptime=true "
        "! appsink name=sink sync=false max-buffers=128 drop=false", &error));
    if (error) { g_error_free(error); CHECK(false); }
    CHECK(pipeline);
    Element source(gst_bin_get_by_name(GST_BIN(pipeline.get()), "source"));
    Element sink(gst_bin_get_by_name(GST_BIN(pipeline.get()), "sink"));
    CHECK(source && sink);
    CHECK(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    constexpr std::array<std::uint32_t, 9> pattern{1, 7, 53, 211, 509, 997, 31, 1921, 3079};
    constexpr std::uint32_t total = 4800;
    std::uint32_t position{}, call{};
    while (position < total) {
        const auto count = std::min(total - position, varied ? pattern[call++ % pattern.size()] : 480U);
        auto* input = gst_buffer_new_allocate(nullptr, count * 6, nullptr);
        CHECK(input);
        GstMapInfo map = GST_MAP_INFO_INIT;
        CHECK(gst_buffer_map(input, &map, GST_MAP_WRITE));
        fill_pcm(map.data, position, count); gst_buffer_unmap(input, &map);
        GST_BUFFER_PTS(input) = gst_util_uint64_scale(position, GST_SECOND, 48000);
        GST_BUFFER_DURATION(input) = gst_util_uint64_scale(position + count, GST_SECOND, 48000) - GST_BUFFER_PTS(input);
        GST_BUFFER_OFFSET(input) = position; GST_BUFFER_OFFSET_END(input) = position + count;
        CHECK(gst_app_src_push_buffer(GST_APP_SRC(source.get()), input) == GST_FLOW_OK);
        position += count;
    }
    CHECK(gst_app_src_end_of_stream(GST_APP_SRC(source.get())) == GST_FLOW_OK);
    std::vector<Buffer> packets;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (std::chrono::steady_clock::now() < deadline) {
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink.get()), 100 * GST_MSECOND);
        if (sample) {
            auto* buffer = gst_sample_get_buffer(sample);
            CHECK(buffer);
            packets.emplace_back(gst_buffer_ref(buffer));
            gst_sample_unref(sample);
            CHECK(packets.size() <= 128);
        } else if (gst_app_sink_is_eos(GST_APP_SINK(sink.get()))) break;
    }
    CHECK(gst_app_sink_is_eos(GST_APP_SINK(sink.get())) && !packets.empty());
    return packets;
}

struct InsertState { std::uint64_t wire{}; unsigned rate{}; bool okay{true}; };
gboolean insert_list(GstBuffer** buffer, guint, gpointer opaque) noexcept {
    auto& state = *static_cast<InsertState*>(opaque);
    try {
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!gst_rtp_buffer_map(*buffer, GST_MAP_READ, &rtp)) { state.okay = false; return FALSE; }
        const auto size = gst_rtp_buffer_get_payload_len(&rtp);
        const auto timestamp = gst_rtp_buffer_get_timestamp(&rtp);
        gst_rtp_buffer_unmap(&rtp);
        if (size == 0 || size % 6 || timestamp != rtp_zero + static_cast<std::uint32_t>(state.wire)) {
            state.okay = false; return FALSE;
        }
        const auto record = record_for(state.wire, state.rate);
        if (!avsync::net::add_audio_anchor(*buffer, record)) { state.okay = false; return FALSE; }
        state.wire += size / 6;
        return TRUE;
    } catch (...) { state.okay = false; return FALSE; }
}

void generated_transport_tests() {
    for (const auto rate : {44100U, 192000U}) for (const bool varied : {false, true}) {
        auto packets = actual_payloader(varied);
        InsertState state{0, rate, true};
        AudioReceiverValidator validator(clock_epoch);
        bool wrapped{}, rational{}, repeated{}, new_anchor{}, aggregated{};
        std::uint64_t compared{};
        for (std::size_t first = 0; first < packets.size(); first += 7) {
            List list(gst_buffer_list_new_sized(7));
            CHECK(list);
            const auto end = std::min(first + 7, packets.size());
            for (auto i = first; i < end; ++i) gst_buffer_list_add(list.get(), packets[i].release());
            CHECK(gst_buffer_list_foreach(list.get(), insert_list, &state) && state.okay);
            for (guint i = 0; i < gst_buffer_list_length(list.get()); ++i) {
                auto* value = gst_buffer_list_get(list.get(), i);
                std::uint32_t ssrc{}, timestamp{}, frames{};
                const auto record = avsync::net::read_audio_anchor(value, ssrc, timestamp, frames);
                CHECK(record);
                CHECK(ssrc == ssrc_value);
                CHECK(record->packet_wire_start == compared);
                CHECK(payload_matches(value, *record));
                const auto result = validator.observe(*record, ssrc, timestamp, frames, arrival(compared));
                CHECK(result.accepted && !validator.faulted());
                repeated = repeated || result.repeated;
                new_anchor = new_anchor || record->anchor_sequence > 0;
                wrapped = wrapped || timestamp < rtp_zero;
                const auto anchor_position = avsync::nominal_wire_position(record->device_position,
                    record->device_origin, 0, rate);
                CHECK(anchor_position);
                rational = rational || anchor_position->remainder != 0;
                if (varied && compared == 0) aggregated = frames > 61; // first three input chunks total61.
                compared += frames;
            }
        }
        CHECK(compared == 4800 && state.wire == 4800 && validator.next_wire_frame() == 4800);
        CHECK(wrapped && rational && repeated && new_anchor);
        CHECK(!varied || aggregated);
    }
}

void fail_closed_tests() {
    auto first = packet(0, 180), second = packet(180, 180), third = packet(360, 180);
    for (const bool duplicate : {false, true}) {
        AudioReceiverValidator receiver(clock_epoch);
        CHECK(deliver(receiver, first.get(), arrival(0)).accepted);
        CHECK(!deliver(receiver, duplicate ? first.get() : third.get(), arrival(360)).accepted);
        CHECK(receiver.faulted());
        CHECK(!deliver(receiver, second.get(), arrival(180)).accepted);
    }
    AudioReceiverValidator reordered(clock_epoch);
    CHECK(!deliver(reordered, second.get(), arrival(180)).accepted);
    CHECK(reordered.faulted());
    AudioReceiverValidator missing(clock_epoch);
    CHECK(deliver(missing, first.get(), arrival(0)).accepted);
    auto plain = packet(180, 180, false);
    CHECK(!deliver(missing, plain.get(), arrival(180)).accepted && missing.faulted());
    AudioReceiverValidator stale(clock_epoch);
    CHECK(deliver(stale, first.get(), arrival(0)).accepted);
    CHECK(!deliver(stale, second.get(), origin_ns + 251'000'000).accepted && stale.faulted());
    AudioReceiverValidator wrong_clock(clock_epoch + 1);
    CHECK(!deliver(wrong_clock, first.get(), arrival(0)).accepted);
    // Known PCM markers expose an intentionally mismatched payload association;
    // metadata consistency alone is NOT authentication or a content checksum.
    auto corrupt = packet(0, 180);
    byte(corrupt.get(), 116, 0x7f);
    std::uint32_t ssrc{}, timestamp{}, frames{};
    const auto record = avsync::net::read_audio_anchor(corrupt.get(), ssrc, timestamp, frames);
    CHECK(record);
    AudioReceiverValidator metadata_only(clock_epoch);
    CHECK(metadata_only.observe(*record, ssrc, timestamp, frames, arrival(0)).accepted);
    CHECK(!payload_matches(corrupt.get(), *record));
    // A carried anchor may not describe samples after this packet starts.
    auto ahead_record = record_for(1080); ahead_record.packet_wire_start = 180;
    auto ahead = packet(180, 180, false);
    auto* value = ahead.release(); CHECK(avsync::net::add_audio_anchor(value, ahead_record)); ahead.reset(value);
    AudioReceiverValidator ahead_receiver(clock_epoch);
    CHECK(deliver(ahead_receiver, first.get(), arrival(0)).accepted);
    CHECK(!deliver(ahead_receiver, ahead.get(), arrival(180)).accepted && ahead_receiver.faulted());
}

Buffer sender_report() {
    Buffer result(gst_rtcp_buffer_new(256));
    CHECK(result);
    GstRTCPBuffer map = GST_RTCP_BUFFER_INIT;
    CHECK(gst_rtcp_buffer_map(result.get(), GST_MAP_READWRITE, &map));
    GstRTCPPacket sr, sdes;
    CHECK(gst_rtcp_buffer_add_packet(&map, GST_RTCP_TYPE_SR, &sr));
    gst_rtcp_packet_sr_set_sender_info(&sr, ssrc_value, 10ULL << 32, rtp_zero, 1, 1080);
    CHECK(gst_rtcp_buffer_add_packet(&map, GST_RTCP_TYPE_SDES, &sdes));
    CHECK(gst_rtcp_packet_sdes_add_item(&sdes, ssrc_value));
    constexpr std::array<guint8, 7> cname{'f','i','x','t','u','r','e'};
    CHECK(gst_rtcp_packet_sdes_add_entry(&sdes, GST_RTCP_SDES_CNAME,
        static_cast<guint8>(cname.size()), cname.data()));
    CHECK(gst_rtcp_buffer_unmap(&map));
    CHECK(gst_rtcp_buffer_validate(result.get()));
    return result;
}

// Synchronous calls into the real rtpsession sink pads impose an exact order
// across RTP and RTCP, unlike two independently scheduled appsrc tasks. No
// sockets, wall-clock pacing, jitter-buffer or live-device timing is involved.
std::vector<std::uint64_t> session_startup(int rtcp_before_packet, bool disable_probation) {
    Pipeline pipeline(gst_pipeline_new("generated-session-startup"));
    CHECK(pipeline);
    auto* session = gst_element_factory_make("rtpsession", "session");
    auto* sink = gst_element_factory_make("appsink", "ordered");
    CHECK(session && sink);
    gst_bin_add_many(GST_BIN(pipeline.get()), session, sink, nullptr);
    guint default_probation{};
    g_object_get(session, "probation", &default_probation, nullptr);
    CHECK(default_probation == 2);
    if (disable_probation) g_object_set(session, "probation", 0U, nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", 8U,
                 "drop", FALSE, "enable-last-sample", FALSE, nullptr);
    Pad rtp(gst_element_request_pad_simple(session, "recv_rtp_sink"));
    Pad rtcp(gst_element_request_pad_simple(session, "recv_rtcp_sink"));
    Pad output(gst_element_get_static_pad(session, "recv_rtp_src"));
    Pad input(gst_element_get_static_pad(sink, "sink"));
    CHECK(rtp && rtcp && output && input);
    CHECK(gst_pad_link(output.get(), input.get()) == GST_PAD_LINK_OK);
    CHECK(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    GstSegment segment; gst_segment_init(&segment, GST_FORMAT_TIME);
    for (auto* pad : {rtp.get(), rtcp.get()}) {
        CHECK(gst_pad_send_event(pad, gst_event_new_stream_start(pad == rtp.get() ? "synthetic-rtp" : "synthetic-rtcp")));
        auto* caps = pad == rtp.get() ? gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "audio", "clock-rate", G_TYPE_INT, 48000,
            "encoding-name", G_TYPE_STRING, "L24", "channels", G_TYPE_INT, 2,
            "encoding-params", G_TYPE_STRING, "2", "payload", G_TYPE_INT, 96, nullptr)
            : gst_caps_new_empty_simple("application/x-rtcp");
        CHECK(gst_pad_send_event(pad, gst_event_new_caps(caps))); gst_caps_unref(caps);
        CHECK(gst_pad_send_event(pad, gst_event_new_segment(&segment)));
    }
    for (int i = 0; i < 3; ++i) {
        if (i == rtcp_before_packet) CHECK(gst_pad_chain(rtcp.get(), sender_report().release()) == GST_FLOW_OK);
        auto value = packet(static_cast<std::uint64_t>(i) * 180, 180);
        GstRTPBuffer map = GST_RTP_BUFFER_INIT;
        CHECK(gst_rtp_buffer_map(value.get(), GST_MAP_READWRITE, &map));
        gst_rtp_buffer_set_seq(&map, static_cast<guint16>(1000 + i));
        gst_rtp_buffer_unmap(&map);
        GST_BUFFER_PTS(value.get()) = static_cast<GstClockTime>(i) * 3'750'000;
        GST_BUFFER_DTS(value.get()) = GST_BUFFER_PTS(value.get());
        CHECK(gst_pad_chain(rtp.get(), value.release()) == GST_FLOW_OK);
    }
    std::vector<std::uint64_t> observed;
    while (auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 0)) {
        auto* value = gst_sample_get_buffer(sample);
        std::uint32_t ssrc{}, timestamp{}, frames{};
        const auto record = avsync::net::read_audio_anchor(value, ssrc, timestamp, frames);
        CHECK(record && frames == 180 && ssrc == ssrc_value);
        CHECK(payload_matches(value, *record));
        observed.push_back(record->packet_wire_start);
        gst_sample_unref(sample);
        CHECK(observed.size() <= 3);
    }
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    gst_element_release_request_pad(session, rtp.get());
    gst_element_release_request_pad(session, rtcp.get());
    return observed;
}

void startup_probation_tests() {
    for (const bool disabled : {false, true}) for (const int ordering : {-1, 0, 1, 2}) {
        const auto observed = session_startup(ordering, disabled);
        std::cout << "generated startup probation=" << (disabled ? 0 : 2)
                  << " rtcp_before_packet=" << ordering << " wire_frames=";
        for (const auto frame : observed) std::cout << frame << ',';
        std::cout << '\n';
        // Both tested versions preserve the queued first packet, including SR
        // between the first two RTP packets. Do not blame probation without
        // independent evidence of a different input sequence.
        CHECK((observed == std::vector<std::uint64_t>{0, 180, 360}));
    }
}

struct JitterResult {
    std::vector<std::uint64_t> observed;
    unsigned capacity_drops{};
};
GstPadProbeReturn saw_segment(GstPad*, GstPadProbeInfo* info, gpointer opaque) noexcept {
    if (GST_EVENT_TYPE(GST_PAD_PROBE_INFO_EVENT(info)) == GST_EVENT_SEGMENT)
        static_cast<std::atomic_bool*>(opaque)->store(true);
    return GST_PAD_PROBE_OK;
}
JitterResult jitter_startup(unsigned packet_count, bool drop_oldest, unsigned faststart) {
    // Callback state outlives pipeline teardown, including failed assertions.
    std::atomic_bool segment_seen{};
    Pipeline pipeline(gst_pipeline_new("generated-jitter-startup"));
    CHECK(pipeline);
    auto* jitter = gst_element_factory_make("rtpjitterbuffer", "jitter");
    auto* sink = gst_element_factory_make("appsink", "ordered");
    CHECK(jitter && sink && packet_count <= 29);
    gst_bin_add_many(GST_BIN(pipeline.get()), jitter, sink, nullptr);
    g_object_set(jitter, "latency", 100U, "drop-on-latency", drop_oldest,
        "faststart-min-packets", faststart, "post-drop-messages", TRUE,
        "drop-messages-interval", 0U,
        // Hold only this fixture's startup deadline far in the future. We
        // release it below; no real/system clock is changed.
        "ts-offset", static_cast<gint64>(30 * GST_SECOND), nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, "max-buffers", 32U,
                 "drop", FALSE, "enable-last-sample", FALSE, nullptr);
    CHECK(gst_element_link(jitter, sink));
    Pad input(gst_element_get_static_pad(jitter, "sink"));
    Pad output(gst_element_get_static_pad(sink, "sink"));
    CHECK(input && output);
    const auto probe = gst_pad_add_probe(output.get(), GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM,
        saw_segment, &segment_seen, nullptr);
    CHECK(probe != 0);
    CHECK(gst_element_set_state(pipeline.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
    CHECK(gst_pad_send_event(input.get(), gst_event_new_stream_start("generated-jitter-rtp")));
    auto* caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio",
        "clock-rate", G_TYPE_INT, 48000, "encoding-name", G_TYPE_STRING, "L24",
        "channels", G_TYPE_INT, 2, "encoding-params", G_TYPE_STRING, "2",
        "payload", G_TYPE_INT, 96, nullptr);
    CHECK(gst_pad_send_event(input.get(), gst_event_new_caps(caps))); gst_caps_unref(caps);
    GstSegment segment; gst_segment_init(&segment, GST_FORMAT_TIME);
    CHECK(gst_pad_send_event(input.get(), gst_event_new_segment(&segment)));
    const auto setup_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!segment_seen.load() && std::chrono::steady_clock::now() < setup_deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    CHECK(segment_seen.load());
    gst_pad_remove_probe(output.get(), probe);
    JitterResult result;
    auto receive = [&](GstClockTime timeout) {
        auto* sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), timeout);
        if (!sample) return false;
        auto* value = gst_sample_get_buffer(sample);
        std::uint32_t ssrc{}, timestamp{}, frames{};
        const auto record = avsync::net::read_audio_anchor(value, ssrc, timestamp, frames);
        CHECK(record && ssrc == ssrc_value && frames == 180);
        CHECK(payload_matches(value, *record));
        result.observed.push_back(record->packet_wire_start);
        gst_sample_unref(sample);
        CHECK(result.observed.size() <= packet_count);
        return true;
    };
    for (unsigned i = 0; i < packet_count; ++i) {
        // A following chain call reschedules the clock wait after this local
        // offset change. Changing ts-offset alone does not wake that wait.
        if (i + 1 == packet_count)
            g_object_set(jitter, "ts-offset", static_cast<gint64>(0), nullptr);
        auto value = packet(static_cast<std::uint64_t>(i) * 180, 180);
        GstRTPBuffer map = GST_RTP_BUFFER_INIT;
        CHECK(gst_rtp_buffer_map(value.get(), GST_MAP_READWRITE, &map));
        gst_rtp_buffer_set_seq(&map, static_cast<guint16>(1000 + i));
        gst_rtp_buffer_unmap(&map);
        GST_BUFFER_PTS(value.get()) = GST_BUFFER_DTS(value.get()) = 0;
        CHECK(gst_pad_chain(input.get(), value.release()) == GST_FLOW_OK);
        if (faststart && i == 1) {
            // Explicit phase barrier: prove origin delivery before the second
            // bounded burst. Not a claim that faststart prevents all overflow
            // if a consumer/worker remains stalled indefinitely.
            CHECK(receive(3 * GST_SECOND)); CHECK(receive(3 * GST_SECOND));
            CHECK((result.observed == std::vector<std::uint64_t>{0, 180}));
        }
    }
    CHECK(gst_pad_send_event(input.get(), gst_event_new_eos()));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (std::chrono::steady_clock::now() < deadline) {
        if (!receive(100 * GST_MSECOND) && gst_app_sink_is_eos(GST_APP_SINK(sink))) break;
    }
    CHECK(gst_app_sink_is_eos(GST_APP_SINK(sink)));
    auto* bus = gst_element_get_bus(pipeline.get());
    while (auto* message = gst_bus_pop_filtered(bus, GST_MESSAGE_ELEMENT)) {
        const auto* structure = gst_message_get_structure(message);
        if (structure && gst_structure_has_name(structure, "drop-msg")) {
            const auto* reason = gst_structure_get_string(structure, "reason");
            if (reason && std::string_view(reason) == "drop-on-latency") ++result.capacity_drops;
        }
        gst_message_unref(message);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    return result;
}

void startup_jitter_tests() {
    const auto under = jitter_startup(27, true, 0);
    const auto overflowing = jitter_startup(29, true, 0);
    const auto retained = jitter_startup(29, false, 0);
    const auto fast = jitter_startup(29, true, 2);
    for (const auto* result : {&under, &overflowing, &retained, &fast}) {
        CHECK(!result->observed.empty());
        for (std::size_t i = 1; i < result->observed.size(); ++i)
            CHECK(result->observed[i] == result->observed[i - 1] + 180);
    }
    std::cout << "generated jitter startup latency_ms=100 held_deadline=true"
        << " under_first=" << under.observed.front() << " under_count=" << under.observed.size()
        << " overflow_first=" << overflowing.observed.front() << " overflow_count=" << overflowing.observed.size()
        << " overflow_drop_messages=" << overflowing.capacity_drops
        << " retained_first=" << retained.observed.front() << " retained_count=" << retained.observed.size()
        << " fast2_first=" << fast.observed.front() << " fast2_count=" << fast.observed.size() << '\n';
    CHECK(under.observed.front() == 0 && under.observed.size() == 27 && under.capacity_drops == 0);
    CHECK(overflowing.observed.front() == 180 && overflowing.observed.size() == 28 && overflowing.capacity_drops == 1);
    CHECK(retained.observed.front() == 0 && retained.observed.size() == 29 && retained.capacity_drops == 0);
    CHECK(fast.observed.front() == 0 && fast.observed.size() == 29 && fast.capacity_drops == 0);
}
} // namespace

int main() {
    try {
        gst_init(nullptr, nullptr);
        adapter_tests(); generated_transport_tests(); fail_closed_tests(); startup_probation_tests(); startup_jitter_tests();
        std::cout << checks << " RTP anchor / generated L24 checks passed; no live capture or network\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RTP anchor tests failed: " << error.what() << '\n';
        return 1;
    }
}
