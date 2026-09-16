// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit finite GENERATED audio only. No WASAPI, microphone or playback device.
#include "avsync/audio_anchors.hpp"
#include "avsync/capture_window.hpp"
#include "avsync/network_clock.hpp"
#include "avsync/process_control.hpp"
#include "avsync/rtp_audio_anchor.hpp"
#include <gst/app/gstappsrc.h>
#include <gst/net/gstnet.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace {
using Steady = std::chrono::steady_clock;
using Ns = avsync::Nanoseconds;
constexpr std::uint32_t rate = 48000, quantum = 480, warmup_frames = 480000, tone_frames = 1440;
constexpr std::array<std::uint32_t, 6> offsets{48000, 112800, 196800, 321600, 487200, 710400};
constexpr avsync::CaptureWindowPolicy capture_window{100'000'000, 100'000'000};
struct Failure { const char* reason; };
void require(bool okay, const char* reason) { if (!okay) throw Failure{reason}; }
struct ObjectDelete { template<class T> void operator()(T* p) const { if (p) gst_object_unref(p); } };
template<class T> using Object = std::unique_ptr<T, ObjectDelete>;
struct PipelineDelete {
    void operator()(GstElement* p) const {
        if (p) { gst_element_set_state(p, GST_STATE_NULL); gst_object_unref(p); }
    }
};
struct BufferDelete { void operator()(GstBuffer* p) const { if (p) gst_buffer_unref(p); } };
using Buffer = std::unique_ptr<GstBuffer, BufferDelete>;

struct Options {
    std::string host;
    unsigned clock_port{}, rtp_port{}, rtcp_port{}, seconds{}, fixture_id{};
    std::uint64_t clock_epoch{}, sender_session{};
    bool control{};
};
template<class T> bool number(std::string_view text, T& value) {
    if (text.empty() || (text.size() > 1 && text.front() == '0')) return false;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool unicast(std::string_view host) {
    std::array<unsigned, 4> parts{};
    for (unsigned i = 0; i < parts.size(); ++i) {
        const auto dot = host.find('.');
        if ((i < 3 && dot == host.npos) || (i == 3 && dot != host.npos)) return false;
        if (!number(dot == host.npos ? host : host.substr(0, dot), parts[i]) || parts[i] > 255) return false;
        if (dot != host.npos) host.remove_prefix(dot + 1);
    }
    return parts[0] != 0 && parts[0] < 224;
}
std::optional<Options> parse(int argc, char** argv) {
    Options value;
    bool generated{};
    unsigned seen{};
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--generated-audio" && !generated) { generated = true; continue; }
        if (arg == "--control-stdin" && !value.control) { value.control = true; continue; }
        if (++i >= argc) return {};
        const std::string_view text(argv[i]);
        unsigned bit{};
        unsigned* field{};
        if (arg == "--host") bit = 1;
        else if (arg == "--clock-port") { bit = 2; field = &value.clock_port; }
        else if (arg == "--rtp-port") { bit = 4; field = &value.rtp_port; }
        else if (arg == "--rtcp-port") { bit = 8; field = &value.rtcp_port; }
        else if (arg == "--seconds") { bit = 16; field = &value.seconds; }
        else if (arg == "--fixture-id") { bit = 32; field = &value.fixture_id; }
        else if (arg == "--clock-epoch") bit = 64;
        else if (arg == "--sender-session") bit = 128;
        else return {};
        if (seen & bit) return {};
        seen |= bit;
        if (field) { if (!number(text, *field)) return {}; }
        else if (bit == 1) value.host = text;
        else if (!number(text, bit == 64 ? value.clock_epoch : value.sender_session)) return {};
    }
    if (!generated || seen != 255 || !unicast(value.host) || !value.clock_epoch || !value.sender_session ||
        value.seconds < 1 || value.seconds > 180 || value.fixture_id < 1 || value.fixture_id > 2) return {};
    const auto port = [](unsigned p) { return p >= 1024 && p <= 65535; };
    if (!port(value.clock_port) || !port(value.rtp_port) || !port(value.rtcp_port) ||
        value.clock_port == value.rtp_port || value.clock_port == value.rtcp_port || value.rtp_port == value.rtcp_port) return {};
    return value;
}
void help() {
    std::cout << "avsync-network-fixture-sender --generated-audio --host IPV4 --clock-port N --rtp-port N --rtcp-port N\n"
                 " --clock-epoch U64 --sender-session U64 --seconds 1..180 --fixture-id 1|2 [--control-stdin]\n"
                 "Explicit GENERATED stereo L24/RTP/RTCP; no devices or operating-system clock changes.\n"
                 "Ten seconds of generated silence precedes six nonuniform 30 ms tones.\n"
                 "Overall deadline includes clock acquisition; short runs may produce no markers.\n"
                 "Original scheduled sample times are mapped once; manifests do not prove delivery.\n"
                 "Control stdin uses KEEPALIVE/STOP, EOF and a five-second lease. No automatic clock-loss reset.\n";
}
Ns signed_time(GstClockTime value) {
    require(GST_CLOCK_TIME_IS_VALID(value) && value <= static_cast<guint64>(std::numeric_limits<Ns>::max()), "clock_range");
    return static_cast<Ns>(value);
}
struct Statistics {
    std::uint64_t generated_packets{}, generated_frames{}, markers{}, mapped_packets{}, clock_losses{};
    std::atomic<std::uint64_t> rtp_packets{}, anchored_packets{}, sender_reports{}, anchor_errors{}, queue_overflows{};
};
void summary(const Statistics& s, const avsync::net::ClockHealth& health, const char* status, const char* reason = "none") {
    std::cout << "{\"schema\":1,\"status\":\"" << status << "\",\"generated_audio\":true,\"reason\":\"" << reason
              << "\",\"generated_packets\":" << s.generated_packets << ",\"generated_frames\":" << s.generated_frames
              << ",\"generated_markers\":" << s.markers << ",\"mapped_packets\":" << s.mapped_packets
              << ",\"rtp_packets_at_output\":" << s.rtp_packets.load() << ",\"anchored_rtp_packets\":" << s.anchored_packets.load()
              << ",\"sender_reports\":" << s.sender_reports.load() << ",\"anchor_transport_errors\":" << s.anchor_errors.load()
              << ",\"queue_overflows\":" << s.queue_overflows.load() << ",\"clock_loss_count\":" << s.clock_losses
              << ",\"clock_usable\":" << (health.usable ? "true" : "false")
              << ",\"original_anchors_transmitted\":" << (s.anchored_packets ? "true" : "false")
              << ",\"media_verified\":false}\n" << std::flush;
}
void header(const Options& o, Ns origin) {
    std::cout << "AVSYNC_FIXTURE {\"schema\":1,\"type\":\"header\",\"fixture_id\":" << o.fixture_id
              << ",\"sender_session\":\"" << o.sender_session << "\",\"clock_epoch\":\"" << o.clock_epoch
              << "\",\"generation\":\"1\",\"sample_rate\":48000,\"channels\":2,\"warmup_frames\":480000,"
                 "\"duration_frames\":1440,\"capture_origin_ns\":\"" << origin << "\",\"frame_offsets\":[";
    for (std::size_t i = 0; i < offsets.size(); ++i) std::cout << (i ? "," : "") << offsets[i];
    std::cout << "]}\n" << std::flush;
}
unsigned frequency(unsigned fixture, unsigned event) { return 660 + 220 * event + 2200 * (fixture - 1); }
void marker(const Options& o, unsigned event, Ns capture) {
    std::cout << "AVSYNC_FIXTURE {\"schema\":1,\"type\":\"marker\",\"fixture_id\":" << o.fixture_id
              << ",\"event\":" << event + 1 << ",\"frame_position\":" << warmup_frames + offsets[event]
              << ",\"capture_ns\":\"" << capture << "\",\"duration_frames\":1440,\"frequency_hz\":"
              << frequency(o.fixture_id, event) << "}\n" << std::flush;
}
void fill(std::array<guint8, quantum * 6>& bytes, std::uint64_t first, unsigned fixture) {
    for (unsigned i = 0; i < quantum; ++i) {
        const auto frame = first + i;
        double value{};
        for (unsigned event = 0; event < offsets.size(); ++event) {
            const auto begin = warmup_frames + offsets[event];
            if (frame < begin || frame >= begin + tone_frames) continue;
            const auto delta = frame - begin;
            const auto envelope = std::min(1.0, double(delta) / 48.0) * std::min(1.0, double(tone_frames - delta) / 48.0);
            value = .20 * envelope * std::sin(6.283185307179586 * frequency(fixture, event) * double(delta) / rate);
        }
        const auto sample = static_cast<std::int32_t>(std::llround(value * 8388607.0));
        const auto encoded = static_cast<std::uint32_t>(sample) & 0xffffffU;
        for (unsigned channel = 0; channel < 2; ++channel) {
            const auto p = i * 6 + channel * 3;
            bytes[p] = static_cast<guint8>(encoded >> 16);
            bytes[p + 1] = static_cast<guint8>(encoded >> 8);
            bytes[p + 2] = static_cast<guint8>(encoded);
        }
    }
}

class Sender {
public:
    Sender(const Options& o, GstClock* clock, Statistics& stats)
        : options_(o), clock_(clock), stats_(stats) {
        pipeline_.reset(gst_pipeline_new("generated-network-fixture"));
        require(pipeline_ != nullptr, "create_pipeline");
        source_ = make("appsrc", "generated-desktop");
        auto* pay = make("rtpL24pay", "generated-l24");
        auto* rtpbin = make("rtpbin", "generated-session");
        auto* rtp_sink = make("udpsink", "rtp-output");
        auto* rtcp_sink = make("udpsink", "rtcp-output");
        g_object_set(source_, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", FALSE,
            "block", FALSE, "max-time", static_cast<guint64>(100 * GST_MSECOND),
            "max-bytes", static_cast<guint64>(rate * 6 / 10), "max-buffers", static_cast<guint64>(32),
            "leaky-type", GST_APP_LEAKY_TYPE_UPSTREAM, "emit-signals", TRUE,
            "min-latency", static_cast<gint64>(0), "max-latency", static_cast<gint64>(100 * GST_MSECOND), nullptr);
        auto* caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S24BE", "layout", G_TYPE_STRING,
            "interleaved", "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2,
            "channel-mask", GST_TYPE_BITMASK, static_cast<guint64>(3), nullptr);
        require(caps != nullptr, "create_caps");
        gst_app_src_set_caps(GST_APP_SRC(source_), caps);
        gst_caps_unref(caps);
        g_signal_connect(source_, "enough-data", G_CALLBACK(enough_data), this);
        guint32 ssrc{};
        while (!ssrc) ssrc = g_random_int();
        g_object_set(pay, "pt", static_cast<guint>(96), "ssrc", ssrc, "mtu", static_cast<guint>(1096),
            "max-ptime", static_cast<gint64>(4 * GST_MSECOND), nullptr);
        g_object_set(rtpbin, "ntp-time-source", 3, "rtcp-sync-send-time", FALSE, nullptr);
        auto* sdes = gst_structure_new("application/x-rtp-source-sdes", "cname", G_TYPE_STRING,
            "avsync-generated-fixture", "tool", G_TYPE_STRING, "av-sync-bridge generated fixture", nullptr);
        require(sdes != nullptr, "create_sdes");
        g_object_set(rtpbin, "sdes", sdes, nullptr);
        gst_structure_free(sdes);
        for (auto* sink : {rtp_sink, rtcp_sink}) g_object_set(sink, "host", o.host.c_str(),
            "port", static_cast<gint>(sink == rtp_sink ? o.rtp_port : o.rtcp_port),
            "sync", FALSE, "async", FALSE, "buffer-size", 16384, nullptr);
        require(gst_element_link(source_, pay), "link_source");
        Object<GstPad> send(gst_element_request_pad_simple(rtpbin, "send_rtp_sink_0"));
        Object<GstPad> paid(gst_element_get_static_pad(pay, "src"));
        require(send && paid && gst_pad_link(paid.get(), send.get()) == GST_PAD_LINK_OK, "link_payloader");
        Object<GstPad> rtp(gst_element_get_static_pad(rtpbin, "send_rtp_src_0"));
        Object<GstPad> rtp_input(gst_element_get_static_pad(rtp_sink, "sink"));
        Object<GstPad> rtcp(gst_element_request_pad_simple(rtpbin, "send_rtcp_src_0"));
        Object<GstPad> rtcp_input(gst_element_get_static_pad(rtcp_sink, "sink"));
        require(rtp && rtp_input && gst_pad_link(rtp.get(), rtp_input.get()) == GST_PAD_LINK_OK, "link_rtp");
        require(rtcp && rtcp_input && gst_pad_link(rtcp.get(), rtcp_input.get()) == GST_PAD_LINK_OK, "link_rtcp");
        require(gst_pad_add_probe(rtp.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST),
            rtp_probe, this, nullptr) != 0, "install_rtp_probe");
        require(gst_pad_add_probe(rtcp.get(), GST_PAD_PROBE_TYPE_BUFFER, rtcp_probe, this, nullptr) != 0, "install_rtcp_probe");
        GObject* session{};
        g_signal_emit_by_name(rtpbin, "get-internal-session", 0, &session);
        require(session != nullptr, "get_rtp_session");
        g_object_set(session, "rtcp-min-interval", static_cast<guint64>(500 * GST_MSECOND), "internal-ssrc", ssrc, nullptr);
        g_object_unref(session);
        gst_pipeline_use_clock(GST_PIPELINE(pipeline_.get()), clock);
        gst_element_set_start_time(pipeline_.get(), GST_CLOCK_TIME_NONE);
        gst_element_set_base_time(pipeline_.get(), 0);
        bus_.reset(gst_element_get_bus(pipeline_.get()));
        require(gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE, "start_pipeline");
    }
    void check() {
        require(!fault_.load(), "anchor_transport_failed");
        require(stats_.queue_overflows.load() == 0, "source_queue_overflow");
        for (unsigned i = 0; i < 64; ++i) {
            auto* message = gst_bus_pop(bus_.get());
            if (!message) break;
            const auto failed = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
            gst_message_unref(message);
            require(!failed, "pipeline_error");
        }
    }
    void push(const avsync::net::MappedCapture& capture, std::uint64_t first, std::uint64_t local_100ns) {
        check();
        const auto now = signed_time(gst_clock_get_time(clock_));
        require(avsync::capture_window_status(capture.capture_ns, now, capture_window) == avsync::CaptureWindowStatus::accepted,
            "capture_window_failed");
        if (!timeline_) {
            avsync::AudioAnchorConfig config;
            config.rate.nominal_rate = rate;
            config.max_anchor_age_ns = 100'000'000;
            config.max_future_ns = 100'000'000;
            anchors_.emplace(avsync::SessionToken{options_.sender_session, 1}, config);
            timeline_.emplace(capture.capture_ns, rate);
            wire_origin_ = capture.capture_ns;
        }
        require(first == pushed_frames_, "source_frame_gap");
        const auto sequence = first / quantum;
        (void)anchors_->observe({{options_.sender_session, 1}, sequence, first, capture.capture_ns, rate, false}, now);
        require(!anchors_->faulted(), "source_anchor_failed");
        avsync::wire::AudioRecord record{{options_.sender_session, 1}, options_.clock_epoch, 0, rate, 0, 0,
            sequence, first, capture.capture_ns, local_100ns, capture.revision};
        {
            std::lock_guard lock(mutex_);
            require(size_ < ledger_.size(), "anchor_ledger_overflow");
            ledger_[(head_ + size_) % ledger_.size()] = record;
            ++size_;
        }
        const auto pts = timeline_->at(first), end = timeline_->at(first + quantum);
        require(pts && end, "nominal_timeline_overflow");
        std::array<guint8, quantum * 6> data{};
        fill(data, first, options_.fixture_id);
        Buffer buffer(gst_buffer_new_allocate(nullptr, data.size(), nullptr));
        require(buffer && gst_buffer_fill(buffer.get(), 0, data.data(), data.size()) == data.size(), "allocate_buffer");
        GST_BUFFER_PTS(buffer.get()) = static_cast<guint64>(*pts);
        GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer.get()) = static_cast<guint64>(*end - *pts);
        GST_BUFFER_OFFSET(buffer.get()) = first;
        GST_BUFFER_OFFSET_END(buffer.get()) = first + quantum;
        if (!first) GST_BUFFER_FLAG_SET(buffer.get(), GST_BUFFER_FLAG_DISCONT);
        require(avsync::capture_window_status(capture.capture_ns, signed_time(gst_clock_get_time(clock_)), capture_window) ==
            avsync::CaptureWindowStatus::accepted, "post_generation_capture_window");
        require(gst_app_src_push_buffer(GST_APP_SRC(source_), buffer.release()) == GST_FLOW_OK, "source_push_failed");
        pushed_frames_ += quantum;
        ++stats_.generated_packets;
        stats_.generated_frames += quantum;
    }
private:
    GstElement* make(const char* factory, const char* name) {
        auto* element = gst_element_factory_make(factory, name);
        require(element != nullptr, "missing_gstreamer_element");
        if (!gst_bin_add(GST_BIN(pipeline_.get()), element)) { gst_object_unref(element); throw Failure{"add_element"}; }
        return element;
    }
    static void enough_data(GstElement*, gpointer opaque) {
        ++static_cast<Sender*>(opaque)->stats_.queue_overflows;
    }
    bool decorate(GstBuffer*& buffer) {
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!buffer || !gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) return false;
        const auto payload = gst_rtp_buffer_get_payload_len(&rtp);
        const auto stamp = gst_rtp_buffer_get_timestamp(&rtp);
        gst_rtp_buffer_unmap(&rtp);
        if (!payload || payload % 6 || payload / 6 > 180) return false;
        if (!wire_frames_) rtp_zero_ = stamp;
        if (stamp != static_cast<guint32>(rtp_zero_ + wire_frames_)) return false;
        avsync::wire::AudioRecord record;
        {
            std::lock_guard lock(mutex_);
            if (!size_) return false;
            while (size_ > 1 && ledger_[(head_ + 1) % ledger_.size()].device_position <= wire_frames_) {
                head_ = (head_ + 1) % ledger_.size();
                --size_;
            }
            record = ledger_[head_];
            const auto expected = avsync::RationalTimeline(wire_origin_, rate).at(wire_frames_);
            const auto actual = GST_BUFFER_PTS(buffer);
            if (!expected || !GST_CLOCK_TIME_IS_VALID(actual) || actual > static_cast<guint64>(std::numeric_limits<Ns>::max()) ||
                std::abs(static_cast<Ns>(actual) - *expected) > 1) return false;
        }
        if ((!advertised_ && record.anchor_sequence != 0) || (advertised_ && record.anchor_sequence != *advertised_ &&
            record.anchor_sequence != *advertised_ + 1)) return false;
        const auto now = gst_clock_get_time(clock_);
        if (!GST_CLOCK_TIME_IS_VALID(now) || now > static_cast<guint64>(std::numeric_limits<Ns>::max()) ||
            avsync::capture_window_status(record.capture_ns, static_cast<Ns>(now), {100'000'000, 200'000'000}) !=
            avsync::CaptureWindowStatus::accepted) return false;
        record.packet_wire_start = wire_frames_;
        record.rtp_zero = rtp_zero_;
        if (!avsync::net::add_audio_anchor(buffer, record)) return false;
        advertised_ = record.anchor_sequence;
        wire_frames_ += payload / 6;
        return true;
    }
    static GstPadProbeReturn rtp_probe(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto& self = *static_cast<Sender*>(opaque);
        if (self.fault_.load()) return GST_PAD_PROBE_DROP;
        try {
            bool valid = false;
            guint count{};
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
                auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                valid = self.decorate(buffer);
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                count = 1;
            } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                auto* list = gst_buffer_list_make_writable(GST_PAD_PROBE_INFO_BUFFER_LIST(info));
                GST_PAD_PROBE_INFO_DATA(info) = list;
                count = list ? gst_buffer_list_length(list) : 0;
                valid = count && count <= 256;
                for (guint i = 0; valid && i < count; ++i) {
                    auto* buffer = gst_buffer_ref(gst_buffer_list_get(list, i));
                    try { valid = self.decorate(buffer); }
                    catch (...) { if (buffer) gst_buffer_unref(buffer); throw; }
                    if (!valid) { if (buffer) gst_buffer_unref(buffer); break; }
                    gst_buffer_list_remove(list, i, 1);
                    gst_buffer_list_insert(list, static_cast<gint>(i), buffer);
                }
            }
            if (valid) {
                self.stats_.rtp_packets.fetch_add(count);
                self.stats_.anchored_packets.fetch_add(count);
                return GST_PAD_PROBE_OK;
            }
        } catch (...) { /* Never unwind across a streaming callback. */ }
        ++self.stats_.anchor_errors;
        self.fault_.store(true);
        return GST_PAD_PROBE_DROP;
    }
    static GstPadProbeReturn rtcp_probe(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto& self = *static_cast<Sender*>(opaque);
        GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
        if (!gst_rtcp_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), GST_MAP_READ, &rtcp)) return GST_PAD_PROBE_OK;
        GstRTCPPacket packet;
        if (gst_rtcp_buffer_get_first_packet(&rtcp, &packet)) do {
            if (gst_rtcp_packet_get_type(&packet) == GST_RTCP_TYPE_SR) ++self.stats_.sender_reports;
        } while (gst_rtcp_packet_move_to_next(&packet));
        gst_rtcp_buffer_unmap(&rtcp);
        return GST_PAD_PROBE_OK;
    }
    const Options& options_;
    GstClock* clock_;
    Statistics& stats_;
    GstElement* source_{};
    Object<GstBus> bus_;
    std::optional<avsync::AudioAnchorTracker> anchors_;
    std::optional<avsync::RationalTimeline> timeline_;
    std::array<avsync::wire::AudioRecord, 32> ledger_{};
    std::mutex mutex_;
    std::size_t head_{}, size_{};
    Ns wire_origin_{};
    std::uint64_t pushed_frames_{}, wire_frames_{};
    guint32 rtp_zero_{};
    std::optional<std::uint64_t> advertised_;
    std::atomic<bool> fault_{};
    // Destroy first, including constructor unwinding, before callback state.
    std::unique_ptr<GstElement, PipelineDelete> pipeline_;
};

void clock_messages(GstBus* bus, avsync::net::ClockHealthMonitor& monitor) {
    for (unsigned i = 0; i < 64; ++i) {
        auto* message = gst_bus_pop(bus);
        if (!message) return;
        (void)monitor.observe(message);
        gst_message_unref(message);
    }
}
int run(const Options& o) {
    Statistics stats;
    avsync::net::ClockHealth health;
    try {
        const auto deadline = Steady::now() + std::chrono::seconds(o.seconds);
        avsync::process::StdinControl control(o.control);
        const auto controlled = [&] {
            summary(stats, health, avsync::process::state_name(control.state()));
            return control.state() == avsync::process::ControlState::stopped ? 0 : 1;
        };
        if (control.poll() != avsync::process::ControlState::active) return controlled();
        GError* error{};
        const bool initialized = gst_init_check(nullptr, nullptr, &error);
        if (error) g_error_free(error);
        require(initialized, "gstreamer_initialize");
        Object<GstBus> clock_bus(gst_bus_new());
        Object<GstClock> clock(gst_net_client_clock_new("generated-shared-clock", o.host.c_str(), static_cast<gint>(o.clock_port), 0));
        require(clock && clock_bus, "create_network_clock");
        g_object_set(clock.get(), "bus", clock_bus.get(), "minimum-update-interval", static_cast<guint64>(100 * GST_MSECOND),
            "round-trip-limit", static_cast<guint64>(5 * GST_MSECOND), nullptr);
        GstClock* raw_internal{};
        g_object_get(clock.get(), "internal-clock", &raw_internal, nullptr);
        Object<GstClock> internal(raw_internal);
        require(internal != nullptr, "missing_internal_clock");
        gst_clock_set_timeout(internal.get(), 250 * GST_MSECOND);
        require(gst_clock_get_timeout(internal.get()) == 250 * GST_MSECOND, "clock_poll_timeout");
        require(avsync::net::verify_local_monotonic_domain(internal.get()).valid, "local_clock_domain");
        if (control.poll() != avsync::process::ControlState::active) return controlled();
        std::cout << "AVSYNC_CONTROL {\"schema\":1,\"event\":\"sender_started\",\"clock_epoch\":\""
                  << o.clock_epoch << "\",\"sender_session\":\"" << o.sender_session << "\"}\n" << std::flush;
        avsync::net::ClockHealthMonitor monitor;
        while (Steady::now() < deadline && control.poll() == avsync::process::ControlState::active) {
            clock_messages(clock_bus.get(), monitor);
            health = monitor.health(clock.get());
            if (health.usable) break;
            gst_clock_wait_for_sync(clock.get(), 20 * GST_MSECOND);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        if (control.state() != avsync::process::ControlState::active) return controlled();
        if (!health.usable || Steady::now() >= deadline) { summary(stats, health, "waiting_clock"); return 3; }
        auto sender = std::make_unique<Sender>(o, clock.get(), stats);
        if (control.poll() != avsync::process::ControlState::active) { sender.reset(); return controlled(); }
        require(Steady::now() < deadline, "deadline_before_generation");
        const auto local_now = signed_time(gst_clock_get_internal_time(internal.get()));
        const auto rounded = avsync::checked_add(local_now, 99);
        require(rounded.has_value(), "source_origin_overflow");
        const auto origin = (*rounded / 100) * 100;
        avsync::RationalTimeline local_timeline(origin, rate);
        avsync::net::CaptureClockMapper mapper;
        std::array<std::optional<Ns>, offsets.size()> marker_times{};
        std::uint64_t first{};
        while (Steady::now() < deadline && control.poll() == avsync::process::ControlState::active) {
            sender->check();
            clock_messages(clock_bus.get(), monitor);
            health = monitor.health(clock.get());
            if (!health.usable) { ++stats.clock_losses; throw Failure{"clock_qualification_lost"}; }
            const auto scheduled = local_timeline.at(first);
            require(scheduled.has_value(), "source_timeline_overflow");
            const auto now = signed_time(gst_clock_get_internal_time(internal.get()));
            if (now < *scheduled) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
            const auto age = avsync::checked_sub(now, *scheduled);
            require(age && *age <= 100'000'000, "source_pacing_stale");
            const auto ticks = static_cast<std::uint64_t>(*scheduled / 100);
            const auto mapped = mapper.map(clock.get(), ticks);
            require(mapped.has_value(), "source_mapping_failed");
            if (!first) header(o, mapped->capture_ns);
            for (unsigned event = 0; event < offsets.size(); ++event)
                if (first == warmup_frames + offsets[event]) marker_times[event] = mapped->capture_ns;
            sender->push(*mapped, first, ticks);
            ++stats.mapped_packets;
            first += quantum;
            for (unsigned event = 0; event < offsets.size(); ++event) {
                if (first == warmup_frames + offsets[event] + tone_frames) {
                    require(marker_times[event].has_value(), "marker_anchor_missing");
                    marker(o, event, *marker_times[event]);
                    ++stats.markers;
                }
            }
        }
        sender->check();
        sender.reset(); // Stop callbacks before final counters; no delayed drain.
        if (control.state() != avsync::process::ControlState::active) return controlled();
        const bool observed = stats.rtp_packets.load() && stats.sender_reports.load();
        summary(stats, health, observed ? "rtp_output_observed_unverified" : "waiting_media");
        return observed ? 0 : 3;
    } catch (const Failure& failure) {
        summary(stats, health, "error", failure.reason);
    } catch (...) {
        summary(stats, health, "error", "fixture_exception");
    }
    return 1;
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) { help(); return 0; }
    const auto options = parse(argc, argv);
    if (!options) { std::cerr << "Invalid generated fixture arguments; no network or devices opened.\n"; return 2; }
    return run(*options);
}
