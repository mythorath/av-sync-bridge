// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded network diagnostics. No audio device, PCM file, IPC bridge or OBS output.
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/net/gstnet.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gio/gio.h>
#include "avsync/network_clock.hpp"
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Steady = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
unsigned number(const char* value, unsigned low, unsigned high) {
    unsigned result{};
    const std::string_view text(value);
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && result >= low && result <= high,
            "Invalid numeric argument");
    return result;
}
struct Options {
    std::string bind, peer;
    unsigned clock_port{}, rtp_port{}, rtcp_port{}, seconds{30};
    unsigned clock_pause_after{}, clock_pause_seconds{};
    bool expect_media{};
};
void help() {
    std::cout << "avsync-network-receiver --bind LOCAL_IPV4 --peer SENDER_IPV4 --clock-port N --rtp-port N --rtcp-port N\n"
                 " [--seconds 1..180] [--expect-media]\n"
                 " [--clock-pause-after SECONDS --clock-pause-seconds 1..10] (test fixture only)\n"
                 "Explicit private-link diagnostic listener and shared monotonic clock provider.\n"
                 "PT96/L24/48kHz/stereo; accepts only the supplied peer's media packets.\n"
                 "No PCM recording/playback, OBS access, firewall changes or startup service.\n";
}
struct InetDeleter { void operator()(GInetAddress* value) const { if(value) g_object_unref(value); } };
using Inet = std::unique_ptr<GInetAddress, InetDeleter>;
Inet address(const std::string& value) {
    Inet result(g_inet_address_new_from_string(value.c_str()));
    require(result && g_inet_address_get_family(result.get()) == G_SOCKET_FAMILY_IPV4 &&
            !g_inet_address_get_is_any(result.get()) && !g_inet_address_get_is_multicast(result.get()) &&
            value != "255.255.255.255", "Use a specific unicast IPv4 address");
    return result;
}
struct ReportSlot {
    bool occupied{};
    guint32 ssrc{};
    std::atomic<GstClockTime> last_received{GST_CLOCK_TIME_NONE};
};
struct Branch {
    ReportSlot* report{};
    std::uint64_t buffers{}, frames{}, untimed{}, untimed_after_lock{}, timed{}, invalid_reference{}, stale_reference{}, nonmonotonic{}, gaps{};
    GstClockTime previous{GST_CLOCK_TIME_NONE};
    GstClockTime first_reference{GST_CLOCK_TIME_NONE};
    std::uint64_t previous_frames{};
    std::int64_t min_age{std::numeric_limits<std::int64_t>::max()}, max_age{std::numeric_limits<std::int64_t>::min()};
    double peak{}, sum_squares{};
    std::uint64_t samples{};
};
class Receiver {
public:
    explicit Receiver(const Options& options) : options_(options), peer_(address(options.peer)) {
        address(options.bind);
        require(options.clock_port != options.rtp_port && options.clock_port != options.rtcp_port &&
                options.rtp_port != options.rtcp_port, "Ports must be distinct");
        require((!options.clock_pause_after && !options.clock_pause_seconds) ||
                (options.clock_pause_after && options.clock_pause_seconds &&
                 options.clock_pause_after + options.clock_pause_seconds < options.seconds),
                "Clock pause needs both options and must finish before the diagnostic deadline");
    }
    ~Receiver() {
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, 5*GST_SECOND);
            gst_object_unref(pipeline_);
        }
        if (provider_) gst_object_unref(provider_);
        if (clock_) gst_object_unref(clock_);
    }
    int run() {
        clock_ = gst_system_clock_obtain();
        require(clock_ != nullptr, "Cannot obtain shared clock");
        GstClockType clock_type;
        g_object_get(clock_, "clock-type", &clock_type, nullptr);
        require(clock_type == GST_CLOCK_TYPE_MONOTONIC, "Provider clock must be monotonic");
        require(avsync::net::verify_local_monotonic_domain(clock_).valid,
                "GStreamer clock does not match the Linux monotonic epoch");
        provider_ = gst_net_time_provider_new(clock_, options_.bind.c_str(), options_.clock_port);
        require(provider_ != nullptr, "Cannot bind clock provider");
        pipeline_ = gst_pipeline_new("avsync-network-diagnostic");
        require(pipeline_ != nullptr, "Cannot create receiver pipeline");
        rtpbin_ = add("rtpbin", "rtp");
        g_object_set(rtpbin_, "latency", 100u, "drop-on-latency", TRUE,
                     "add-reference-timestamp-meta", TRUE, "ntp-sync", FALSE, nullptr);
        g_signal_connect(rtpbin_, "pad-added", G_CALLBACK(pad_added), this);
        auto* rtp = add("udpsrc", "rtp-input");
        auto* rtcp = add("udpsrc", "rtcp-input");
        for (auto* source : {rtp, rtcp})
            g_object_set(source, "address", options_.bind.c_str(), "auto-multicast", FALSE,
                         "reuse", FALSE, "retrieve-sender-address", TRUE, nullptr);
        g_object_set(rtp, "port", static_cast<int>(options_.rtp_port), nullptr);
        g_object_set(rtcp, "port", static_cast<int>(options_.rtcp_port), nullptr);
        auto* caps = gst_caps_new_simple("application/x-rtp", "media", G_TYPE_STRING, "audio",
            "clock-rate", G_TYPE_INT, 48000, "encoding-name", G_TYPE_STRING, "L24",
            "channels", G_TYPE_INT, 2, "encoding-params", G_TYPE_STRING, "2", "payload", G_TYPE_INT, 96, nullptr);
        g_object_set(rtp, "caps", caps, nullptr);
        gst_caps_unref(caps);
        caps = gst_caps_new_empty_simple("application/x-rtcp");
        g_object_set(rtcp, "caps", caps, nullptr);
        gst_caps_unref(caps);
        connect_input(rtp, "recv_rtp_sink_0");
        connect_input(rtcp, "recv_rtcp_sink_0");
        gst_pipeline_use_clock(GST_PIPELINE(pipeline_), clock_);
        gst_element_set_start_time(pipeline_, GST_CLOCK_TIME_NONE);
        gst_element_set_base_time(pipeline_, 0);
        require(gst_element_set_state(pipeline_, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "Cannot start receiver");
        auto* bus = gst_element_get_bus(pipeline_);
        const auto start = Steady::now();
        const auto deadline = start + std::chrono::seconds(options_.seconds);
        std::cout << "AVSYNC_NETWORK_READY clock_domain=linux_monotonic metadata_only=true\n" << std::flush;
        bool error = false;
        bool clock_paused = false, clock_resumed = false;
        while (Steady::now() < deadline && !fatal_.load()) {
            if (options_.clock_pause_after) {
                const auto elapsed = Steady::now()-start;
                if (!clock_paused && elapsed >= std::chrono::seconds(options_.clock_pause_after)) {
                    g_object_set(provider_, "active", FALSE, nullptr);
                    clock_paused = true;
                }
                if (clock_paused && !clock_resumed && elapsed >=
                    std::chrono::seconds(options_.clock_pause_after + options_.clock_pause_seconds)) {
                    g_object_set(provider_, "active", TRUE, nullptr);
                    clock_resumed = true;
                }
            }
            auto* message = gst_bus_timed_pop_filtered(bus, 100*GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING | GST_MESSAGE_EOS));
            if (!message) continue;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                // Error text may contain peer paths/addresses. Keep public diagnostics generic.
                std::cerr << "Receiver pipeline reported an error\n";
                error = true;
            }
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) ++warnings_;
            gst_message_unref(message);
            if (error) break;
        }
        gst_object_unref(bus);
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_element_get_state(pipeline_, nullptr, nullptr, 5*GST_SECOND);
        std::uint64_t timed = 0, invalid = 0, buffers = 0;
        std::cout << "{\"schema\":1,\"mode\":\"rtp_diagnostic\",\"pcm_saved\":false,\"obs_used\":false,"
                  << "\"capture_timing_verified\":false,\"reference_semantics\":\"sender_media_time_not_proof_of_capture_time\","
                  << "\"packets_accepted\":" << accepted_.load() << ",\"packets_rejected\":" << rejected_.load()
                  << ",\"invalid_sender_reports\":" << invalid_reports_.load()
                  << ",\"clock_pause_fixture\":" << (clock_paused ? "true" : "false")
                  << ",\"clock_resumed\":" << (clock_resumed ? "true" : "false")
                  << ",\"warnings\":" << warnings_ << ",\"sessions\":[";
        for (std::size_t n = 0; n < branches_.size(); ++n) {
            const auto& b = *branches_[n];
            timed += b.timed; buffers += b.buffers;
            invalid += b.invalid_reference + b.stale_reference + b.nonmonotonic + b.untimed_after_lock;
            if (n) std::cout << ',';
            std::cout << "{\"index\":" << n << ",\"buffers\":" << b.buffers << ",\"frames\":" << b.frames
                      << ",\"priming_untimed\":" << b.untimed << ",\"timed\":" << b.timed
                      << ",\"untimed_after_lock\":" << b.untimed_after_lock
                      << ",\"invalid_reference\":" << b.invalid_reference << ",\"nonmonotonic\":" << b.nonmonotonic
                      << ",\"stale_reference\":" << b.stale_reference
                      << ",\"gaps_over_2ms\":" << b.gaps << ",\"peak\":" << b.peak
                      << ",\"first_reference_ns\":" << (b.timed ? b.first_reference : 0)
                      << ",\"last_reference_ns\":" << (b.timed ? b.previous : 0)
                      << ",\"rms\":" << (b.samples ? std::sqrt(b.sum_squares/b.samples) : 0)
                      << ",\"min_reference_age_ms\":" << (b.timed ? b.min_age/1e6 : 0)
                      << ",\"max_reference_age_ms\":" << (b.timed ? b.max_age/1e6 : 0) << '}';
        }
        std::cout << "],\"status\":\"" << (error || fatal_ ? "error" : !buffers ? "waiting_media" :
                    !timed ? "priming_reference" : invalid ? "invalid_timing" : "timestamps_observed") << "\"}\n";
        return error || fatal_ ? 1 : options_.expect_media && (!timed || invalid) ? 3 : 0;
    }
private:
    GstElement* add(const char* factory, const char* name) {
        auto* element = gst_element_factory_make(factory, name);
        require(element != nullptr, "Required GStreamer element is unavailable");
        if (!gst_bin_add(GST_BIN(pipeline_), element)) { gst_object_unref(element); throw std::runtime_error("Cannot add element"); }
        return element;
    }
    static GstPadProbeReturn filter_peer(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto* self = static_cast<Receiver*>(opaque);
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        auto* meta = buffer ? gst_buffer_get_net_address_meta(buffer) : nullptr;
        if (!meta || !G_IS_INET_SOCKET_ADDRESS(meta->addr) ||
            !g_inet_address_equal(self->peer_.get(), g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(meta->addr)))) {
            ++self->rejected_; return GST_PAD_PROBE_DROP;
        }
        ++self->accepted_;
        return GST_PAD_PROBE_OK;
    }
    void connect_input(GstElement* source, const char* requested) {
        auto* src = gst_element_get_static_pad(source, "src");
        auto* sink = gst_element_request_pad_simple(rtpbin_, requested);
        require(src && sink, "Cannot obtain RTP input pads");
        gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, filter_peer, this, nullptr);
        if (std::string_view(requested) == "recv_rtcp_sink_0")
            gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, observe_rtcp, this, nullptr);
        const auto linked = gst_pad_link(src, sink);
        gst_object_unref(src); gst_object_unref(sink);
        require(linked == GST_PAD_LINK_OK, "Cannot link RTP inputs");
    }
    ReportSlot* report_slot(guint32 ssrc) {
        std::lock_guard lock(report_mutex_);
        for (auto& slot : reports_) if (slot.occupied && slot.ssrc == ssrc) return &slot;
        for (auto& slot : reports_) if (!slot.occupied) {
            slot.ssrc = ssrc; slot.occupied = true; return &slot;
        }
        fatal_.store(true);
        return nullptr;
    }
    static GstPadProbeReturn observe_rtcp(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto* self = static_cast<Receiver*>(opaque);
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer || !gst_rtcp_buffer_validate_reduced(buffer)) {
            ++self->invalid_reports_; return GST_PAD_PROBE_DROP;
        }
        GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
        if (!gst_rtcp_buffer_map(buffer, GST_MAP_READ, &rtcp)) return GST_PAD_PROBE_DROP;
        GstRTCPPacket packet;
        if (gst_rtcp_buffer_get_first_packet(&rtcp, &packet)) do {
            if (gst_rtcp_packet_get_type(&packet) == GST_RTCP_TYPE_SR) {
                guint32 ssrc, rtp, count, octets; guint64 clock;
                gst_rtcp_packet_sr_get_sender_info(&packet, &ssrc, &clock, &rtp, &count, &octets);
                const auto stamp = (clock >> 32)*GST_SECOND +
                    gst_util_uint64_scale(clock & 0xffffffffULL, GST_SECOND, 1ULL << 32);
                const auto now = gst_util_get_timestamp();
                const bool plausible = stamp <= now ? now-stamp <= 5*GST_SECOND : stamp-now <= 100*GST_MSECOND;
                if (!plausible) { ++self->invalid_reports_; continue; }
                if (auto* slot = self->report_slot(ssrc)) slot->last_received.store(now);
            }
        } while (gst_rtcp_packet_move_to_next(&packet));
        gst_rtcp_buffer_unmap(&rtcp);
        return GST_PAD_PROBE_OK;
    }
    static void pad_added(GstElement*, GstPad* pad, gpointer opaque) {
        auto* self = static_cast<Receiver*>(opaque);
        const std::string_view name(GST_PAD_NAME(pad));
        if (!name.starts_with("recv_rtp_src_0_")) return;
        try {
            std::lock_guard lock(self->branch_mutex_);
            require(self->branches_.size() < 8, "Session limit reached");
            auto branch = std::make_unique<Branch>();
            auto* state = branch.get();
            const auto suffix = name.substr(std::string_view("recv_rtp_src_0_").size());
            const auto separator = suffix.find('_');
            guint32 ssrc{};
            require(separator != suffix.npos, "Invalid session pad name");
            const auto parsed = std::from_chars(suffix.data(), suffix.data()+separator, ssrc);
            require(parsed.ec == std::errc{} && parsed.ptr == suffix.data()+separator, "Invalid session identifier");
            state->report = self->report_slot(ssrc);
            require(state->report != nullptr, "Report session limit reached");
            auto* depay = self->add("rtpL24depay", nullptr);
            auto* sink = self->add("appsink", nullptr);
            g_object_set(sink, "sync", FALSE, "async", FALSE, "emit-signals", TRUE,
                         "max-buffers", 32u, "drop", TRUE, "enable-last-sample", FALSE, nullptr);
            auto* raw_caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S24BE",
                "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, "layout", G_TYPE_STRING, "interleaved", nullptr);
            gst_app_sink_set_caps(GST_APP_SINK(sink), raw_caps);
            gst_caps_unref(raw_caps);
            // Retain callback state even if a later linking/state operation fails.
            self->branches_.push_back(std::move(branch));
            g_signal_connect(sink, "new-sample", G_CALLBACK(new_sample), state);
            require(gst_element_link(depay, sink), "Cannot link audio depayloader");
            auto* depay_sink = gst_element_get_static_pad(depay, "sink");
            const auto linked = gst_pad_link(pad, depay_sink);
            gst_object_unref(depay_sink);
            require(linked == GST_PAD_LINK_OK, "Cannot connect received session");
            require(gst_element_sync_state_with_parent(sink) && gst_element_sync_state_with_parent(depay),
                    "Cannot activate received session");
        } catch (...) { self->fatal_.store(true); }
    }
    static GstFlowReturn new_sample(GstAppSink* sink, gpointer opaque) {
        auto& b = *static_cast<Branch*>(opaque);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        auto* buffer = gst_sample_get_buffer(sample);
        GstMapInfo map{};
        if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) { gst_sample_unref(sample); return GST_FLOW_ERROR; }
        ++b.buffers;
        const auto frames = map.size / 6;
        b.frames += frames;
        if (map.size % 6) ++b.invalid_reference;
        for (std::size_t i=0; i+2<map.size; i+=3) {
            std::int32_t value = (std::uint32_t(map.data[i])<<16) | (std::uint32_t(map.data[i+1])<<8) | map.data[i+2];
            if (value & 0x800000) value -= 0x1000000;
            const auto normalized = double(value) / 8388608.0;
            b.peak = std::max(b.peak, std::abs(normalized)); b.sum_squares += normalized*normalized; ++b.samples;
        }
        auto* reference = gst_buffer_get_reference_timestamp_meta(buffer, nullptr);
        const auto now = gst_util_get_timestamp();
        const auto last_report = b.report ? b.report->last_received.load() : GST_CLOCK_TIME_NONE;
        if (!reference) { if (b.timed) ++b.untimed_after_lock; else ++b.untimed; }
        else if (!GST_CLOCK_TIME_IS_VALID(last_report) || now < last_report || now-last_report > 2*GST_SECOND)
            ++b.stale_reference;
        else if (!gst_caps_is_empty(reference->reference) &&
                 std::string_view(gst_structure_get_name(gst_caps_get_structure(reference->reference,0))) == "timestamp/x-ntp" &&
                 GST_CLOCK_TIME_IS_VALID(reference->timestamp) && reference->timestamp <= std::numeric_limits<std::int64_t>::max() &&
                 (reference->timestamp <= now ? now-reference->timestamp <= 5*GST_SECOND : reference->timestamp-now <= 100*GST_MSECOND)) {
            const auto stamp = reference->timestamp;
            const auto age = static_cast<std::int64_t>(now) - static_cast<std::int64_t>(stamp);
            if (!b.timed) b.first_reference = stamp;
            ++b.timed; b.min_age = std::min(b.min_age,age); b.max_age = std::max(b.max_age,age);
            if (GST_CLOCK_TIME_IS_VALID(b.previous)) {
                if (stamp <= b.previous) ++b.nonmonotonic;
                const auto expected = b.previous + gst_util_uint64_scale(b.previous_frames,GST_SECOND,48000);
                const auto delta = stamp > expected ? stamp-expected : expected-stamp;
                if (delta > 2*GST_MSECOND) ++b.gaps;
            }
            b.previous = stamp; b.previous_frames = frames;
        } else ++b.invalid_reference;
        gst_buffer_unmap(buffer,&map); gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    Options options_;
    Inet peer_;
    GstElement *pipeline_{}, *rtpbin_{};
    GstClock* clock_{};
    GstNetTimeProvider* provider_{};
    std::mutex branch_mutex_;
    std::mutex report_mutex_;
    std::array<ReportSlot, 8> reports_;
    std::vector<std::unique_ptr<Branch>> branches_;
    std::atomic<bool> fatal_{false};
    std::atomic<std::uint64_t> accepted_{0}, rejected_{0};
    std::atomic<std::uint64_t> invalid_reports_{0};
    std::uint64_t warnings_{};
};
}
int main(int argc,char** argv) {
    if (argc==1 || (argc==2 && std::string_view(argv[1])=="--help")) { help(); return 0; }
    try {
        Options options;
        for (int i=1;i<argc;++i) {
            const std::string_view arg(argv[i]);
            if(arg=="--expect-media") { options.expect_media=true; continue; }
            require(i+1<argc,"Missing option value");
            const auto* value=argv[++i];
            if(arg=="--bind") options.bind=value;
            else if(arg=="--peer") options.peer=value;
            else if(arg=="--clock-port") options.clock_port=number(value,1024,65535);
            else if(arg=="--rtp-port") options.rtp_port=number(value,1024,65535);
            else if(arg=="--rtcp-port") options.rtcp_port=number(value,1024,65535);
            else if(arg=="--seconds") options.seconds=number(value,1,180);
            else if(arg=="--clock-pause-after") options.clock_pause_after=number(value,1,179);
            else if(arg=="--clock-pause-seconds") options.clock_pause_seconds=number(value,1,10);
            else throw std::runtime_error("Unknown option");
        }
        require(!options.bind.empty() && !options.peer.empty() && options.clock_port && options.rtp_port && options.rtcp_port,
                "Explicit bind, peer and three ports are required");
        gst_init(nullptr,nullptr);
        Receiver receiver(options);
        return receiver.run();
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
