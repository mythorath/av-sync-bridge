// SPDX-License-Identifier: GPL-2.0-or-later
// Bounded network diagnostics; explicit optional desktop IPC, never OBS changes.
#include <gst/gst.h>
#include <gst/app/gstappsink.h>
#include <gst/net/gstnet.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <gio/gio.h>
#include "avsync/network_clock.hpp"
#include "avsync/rtp_audio_anchor.hpp"
#include "avsync/process_control.hpp"
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
#include "avsync/audio_diagnostic.hpp"
#endif
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
#include "avsync/audio_handoff.hpp"
#include <filesystem>
#endif
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
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Steady = std::chrono::steady_clock;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
avsync::Nanoseconds monotonic_now() {
    const auto value=gst_util_get_timestamp();
    require(value<=static_cast<guint64>(std::numeric_limits<avsync::Nanoseconds>::max()),
            "Local monotonic timestamp overflow");
    return static_cast<avsync::Nanoseconds>(value);
}
unsigned number(const char* value, unsigned low, unsigned high) {
    unsigned result{};
    const std::string_view text(value);
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && result >= low && result <= high,
            "Invalid numeric argument");
    return result;
}
std::uint64_t identity(const char* value) {
    std::uint64_t result{};
    const std::string_view text(value);
    const auto parsed = std::from_chars(text.data(), text.data()+text.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size() && result,
            "Identity must be a nonzero unsigned 64-bit decimal integer");
    return result;
}
struct Options {
    std::string bind, peer, desktop_ipc;
    unsigned clock_port{}, rtp_port{}, rtcp_port{}, seconds{30};
    unsigned clock_pause_after{}, clock_pause_seconds{};
    bool expect_media{}, expect_anchors{}, correct_desktop{}, recover_desktop_ipc{};
    bool control_stdin{}, replace_desktop_ipc{};
    std::optional<std::uint64_t> expected_sender_session;
};
void help() {
    std::cout << "avsync-network-receiver --bind LOCAL_IPV4 --peer SENDER_IPV4 --clock-port N --rtp-port N --rtcp-port N\n"
                 " [--seconds 1..180] [--expect-media] [--expect-anchors]\n"
                 " [--expect-sender-session N] (pin an explicit nonzero sender identity; requires anchors)\n"
                 " [--control-stdin] (pinned process agreement, 5-second KEEPALIVE/STOP pipe lease)\n"
                 " [--replace-desktop-ipc] (requires pinned control mode; retires validated previous IPC v2)\n"
                 " [--correct-desktop] (optional ASRC build; inspect/discard PCM, NEVER playback/OBS)\n"
                 " [--desktop-ipc NEW_PRIVATE_FILE] (explicit IPC+ASRC build; 2-second desktop buffer, no OBS changes)\n"
                 " [--recover-desktop-ipc] (opt in to at most 8 generations from the SAME sender session)\n"
                 " [--clock-pause-after SECONDS --clock-pause-seconds 1..10] (test fixture only)\n"
                 "Explicit private-link diagnostic listener and shared monotonic clock provider.\n"
                 "PT96/L24/48kHz/stereo; accepts only the supplied peer's media packets.\n"
                 "For anchor tests, copy the fresh clock_epoch from READY to the sender.\n"
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
    // Strict-profile RTP observed after the peer filter but before rtpbin.
    // First/last refer to ingress arrival order, not minimum/maximum positions.
    std::atomic<std::uint64_t> ingress_rtp_packets{}, ingress_rtp_frames{};
    std::atomic<std::uint64_t> ingress_first_wire_start{}, ingress_last_wire_start{};
};
class Receiver;
struct Branch {
    Receiver* owner{};
    std::optional<avsync::wire::AudioReceiverValidator> validator;
    std::uint64_t original_packets{}, original_repeats{}, original_measurements{}, original_out_of_range{};
    std::uint64_t original_usable_measurements{};
    bool last_original_estimate_within_limit{};
    std::uint64_t original_invalid{}, original_retired{}, original_frames{};
    std::uint64_t original_callbacks{};
    std::optional<std::uint64_t> first_callback_wire_start, first_callback_anchor_sequence;
    std::optional<unsigned> first_original_failure_status;
    std::optional<std::uint64_t> first_failure_wire_start, first_failure_anchor_sequence;
    std::optional<std::int64_t> first_failure_capture_age_ns;
    unsigned last_original_status{};
    double last_original_ppm{};
    std::optional<avsync::wire::AudioRecord> first_original, last_original;
    ReportSlot* report{};
    std::uint64_t buffers{}, frames{}, untimed{}, untimed_after_lock{}, timed{}, invalid_reference{}, stale_reference{}, nonmonotonic{}, gaps{};
    GstClockTime previous{GST_CLOCK_TIME_NONE};
    GstClockTime first_reference{GST_CLOCK_TIME_NONE};
    std::uint64_t previous_frames{};
    std::int64_t min_age{std::numeric_limits<std::int64_t>::max()}, max_age{std::numeric_limits<std::int64_t>::min()};
    double peak{}, sum_squares{};
    std::uint64_t samples{};
};
template<class T> void json_optional(const std::optional<T>& value) {
    if (value) std::cout << *value;
    else std::cout << "null";
}
void remember_original_failure(Branch& branch, avsync::wire::AudioWireStatus status,
        const std::optional<avsync::wire::AudioRecord>& record, GstClockTime now) noexcept {
    if (branch.first_original_failure_status) return;
    branch.first_original_failure_status = static_cast<unsigned>(status);
    if (!record) return;
    branch.first_failure_wire_start = record->packet_wire_start;
    branch.first_failure_anchor_sequence = record->anchor_sequence;
    if (now <= static_cast<guint64>(std::numeric_limits<std::int64_t>::max()))
        branch.first_failure_capture_age_ns = avsync::checked_sub(
            static_cast<std::int64_t>(now), record->capture_ns);
}
class Receiver {
public:
    explicit Receiver(const Options& options) : options_(options), control_(options.control_stdin), peer_(address(options.peer)) {
        address(options.bind);
        require(options.clock_port != options.rtp_port && options.clock_port != options.rtcp_port &&
                options.rtp_port != options.rtcp_port, "Ports must be distinct");
        require((!options.clock_pause_after && !options.clock_pause_seconds) ||
                (options.clock_pause_after && options.clock_pause_seconds &&
                 options.clock_pause_after + options.clock_pause_seconds < options.seconds),
                "Clock pause needs both options and must finish before the diagnostic deadline");
        require(!options_.recover_desktop_ipc || !options_.desktop_ipc.empty(),
                "Desktop recovery requires explicit desktop IPC output");
        do { clock_epoch_ = (static_cast<std::uint64_t>(g_random_int()) << 32) | g_random_int(); } while (!clock_epoch_);
        admission_.emplace(clock_epoch_, options_.expected_sender_session);
        if (options_.expected_sender_session)
            pinned_ingress_.emplace(clock_epoch_, *options_.expected_sender_session);
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
        avsync::DiagnosticAudioOutput destination;
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
        // Honor buffered cancellation/EOF before mutating any prior mapping.
        if (!options_.desktop_ipc.empty() && control_.poll() == avsync::process::ControlState::active) {
            require(options_.replace_desktop_ipc || !std::filesystem::exists(options_.desktop_ipc),
                    "Desktop IPC requires a new private file or explicit supervised replacement");
            handoff_=std::make_unique<avsync::CorrectedAudioHandoff>(options_.desktop_ipc,2'000'000'000,
                options_.replace_desktop_ipc ? avsync::ipc::ReplacementPolicy::retire_previous :
                                              avsync::ipc::ReplacementPolicy::atomic_replace);
            destination={this,[](void* p,const avsync::CorrectedAudio& block,
                    std::span<const float> pcm,avsync::Nanoseconds now) noexcept {
                auto* self=static_cast<Receiver*>(p);
                return self->handoff_ && self->handoff_->consume(block,pcm,now);
            },[](void* p) noexcept {
                auto* self=static_cast<Receiver*>(p); if (self->handoff_) self->handoff_->revoke();
            },[](void* p,avsync::SessionToken) noexcept {
                auto* self=static_cast<Receiver*>(p);
                if (self->control_.poll() != avsync::process::ControlState::active) return false;
                if (self->handoff_ && self->handoff_->valid()) return true;
                try {
                    if (self->handoff_) {
                        self->retired_ipc_frames_+=self->handoff_->published_frames();
                        self->retired_ipc_busy_+=self->handoff_->busy_retries();
                        self->retired_ipc_peak_=std::max(self->retired_ipc_peak_,self->handoff_->queue_peak());
                        self->handoff_.reset();
                    }
                    self->handoff_=std::make_unique<avsync::CorrectedAudioHandoff>(self->options_.desktop_ipc,2'000'000'000,
                        self->options_.replace_desktop_ipc ? avsync::ipc::ReplacementPolicy::retire_previous :
                                                          avsync::ipc::ReplacementPolicy::atomic_replace);
                    ++self->ipc_recreations_; return true;
                } catch (...) { return false; }
            }};
        }
#else
        require(options_.desktop_ipc.empty(),"Desktop IPC requires an explicit IPC+ASRC build");
#endif
        if (options_.correct_desktop) correction_=std::make_unique<avsync::AudioCorrectionDiagnostic>(clock_epoch_,destination,
            options_.recover_desktop_ipc ? avsync::DesktopRecovery::same_sender_session:avsync::DesktopRecovery::disabled);
#else
        require(!options_.correct_desktop,"Desktop correction requires an explicit optional ASRC build");
#endif
    }
    ~Receiver() {
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
        if (handoff_) handoff_->revoke(); // Also covers startup exceptions/control cancellation.
#endif
        if (pipeline_) {
            gst_element_set_state(pipeline_, GST_STATE_NULL);
            gst_element_get_state(pipeline_, nullptr, nullptr, 5*GST_SECOND);
            gst_object_unref(pipeline_);
        }
        if (provider_) gst_object_unref(provider_);
        if (clock_) gst_object_unref(clock_);
    }
    int run() {
        const auto run_started = Steady::now();
        if (control_.poll() != avsync::process::ControlState::active) return early_control_exit();
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
        g_signal_connect(rtpbin_, "new-jitterbuffer", G_CALLBACK(new_jitterbuffer), this);
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
        if (options_.expected_sender_session) {
            GstState current = GST_STATE_NULL, pending = GST_STATE_VOID_PENDING;
            const auto ready_deadline = run_started + std::chrono::seconds(std::min(options_.seconds, 5u));
            GstStateChangeReturn ready = GST_STATE_CHANGE_ASYNC;
            do {
                if (control_.poll() != avsync::process::ControlState::active) return early_control_exit();
                ready = gst_element_get_state(pipeline_, &current, &pending, 20 * GST_MSECOND);
            } while (ready == GST_STATE_CHANGE_ASYNC && Steady::now() < ready_deadline);
            require((ready == GST_STATE_CHANGE_SUCCESS || ready == GST_STATE_CHANGE_NO_PREROLL) &&
                    current == GST_STATE_PLAYING, "Receiver startup state did not become ready");
        }
        if (control_.poll() != avsync::process::ControlState::active) return early_control_exit();
        auto* bus = gst_element_get_bus(pipeline_);
        if (options_.expected_sender_session) {
            bool startup_error = false, drained = false;
            for (unsigned count = 0; count < 256; ++count) {
                auto* message = gst_bus_pop(bus);
                if (!message) { drained = true; break; }
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS)
                    startup_error = true;
                if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) ++warnings_;
                gst_message_unref(message);
            }
            if (startup_error || !drained || fatal_.load()) {
                gst_object_unref(bus);
                throw std::runtime_error("Receiver startup reported an error or exceeded its message budget");
            }
        }
        const auto start = Steady::now();
        const auto deadline = (options_.control_stdin ? run_started : start) + std::chrono::seconds(options_.seconds);
        std::cout << "AVSYNC_NETWORK_READY clock_domain=linux_monotonic pcm_saved="
                  <<(options_.desktop_ipc.empty()?"false":"true")<<" clock_epoch="
                  << clock_epoch_ << "\n" << std::flush;
        if (options_.expected_sender_session)
            std::cout << "AVSYNC_CONTROL {\"schema\":1,\"event\":\"receiver_ready\",\"clock_epoch\":\""
                      << clock_epoch_ << "\",\"sender_session\":\"" << *options_.expected_sender_session
                      << "\"}\n" << std::flush;
        bool error = false;
        bool clock_paused = false, clock_resumed = false;
        std::optional<avsync::Nanoseconds> clock_pause_ns;
        while (Steady::now() < deadline && !fatal_.load() &&
               control_.poll() == avsync::process::ControlState::active) {
            if (options_.clock_pause_after) {
                const auto elapsed = Steady::now()-start;
                if (!clock_paused && elapsed >= std::chrono::seconds(options_.clock_pause_after)) {
                    g_object_set(provider_, "active", FALSE, nullptr);
                    clock_pause_ns=monotonic_now();
                    clock_paused = true;
                }
                if (clock_paused && !clock_resumed && elapsed >=
                    std::chrono::seconds(options_.clock_pause_after + options_.clock_pause_seconds)) {
                    g_object_set(provider_, "active", TRUE, nullptr);
                    clock_resumed = true;
                }
            }
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
            if (correction_) {
                correction_->tick(monotonic_now(),!clock_paused || clock_resumed);
                if (correction_->failed()) { error=true; break; }
            }
#endif
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
            if (handoff_ && !correction_->awaiting_generation() && !handoff_->heartbeat(monotonic_now())) { error=true; break; }
#endif
            auto* message = gst_bus_timed_pop_filtered(bus, (options_.correct_desktop ? 2:100)*GST_MSECOND,
                static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_WARNING |
                    GST_MESSAGE_EOS | GST_MESSAGE_ELEMENT));
            if (!message) continue;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR) {
                // Error text may contain peer paths/addresses. Keep public diagnostics generic.
                std::cerr << "Receiver pipeline reported an error\n";
                error = true;
            }
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_WARNING) ++warnings_;
            if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ELEMENT) observe_drop_message(message);
            gst_message_unref(message);
            if (error) break;
        }
        gst_object_unref(bus);
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
        if (handoff_) handoff_->revoke(); // Close the buffer before transport teardown.
#endif
        gst_element_set_state(pipeline_, GST_STATE_NULL);
        gst_element_get_state(pipeline_, nullptr, nullptr, 5*GST_SECOND);
        std::optional<avsync::SessionToken> active_epoch;
        std::optional<std::uint32_t> active_ssrc;
        {
            std::lock_guard lock(admission_mutex_);
            active_epoch = admission_->active_epoch();
            active_ssrc = admission_->active_ssrc();
        }
        std::uint64_t timed = 0, invalid = 0, buffers = 0, original_invalid = 0;
        std::uint64_t active_branches = 0, active_windows = 0, active_usable_windows = 0;
        bool active_last_estimate_within_limit = false;
        std::cout << "{\"schema\":1,\"mode\":\"rtp_diagnostic\",\"pcm_saved\":"
                  <<(options_.desktop_ipc.empty()?"false":"true")<<",\"obs_used\":false,"
                  << "\"capture_timing_verified\":false,\"reference_semantics\":\"sender_media_time_not_proof_of_capture_time\","
                  << "\"original_anchor_mode\":" << (options_.expect_anchors ? "true" : "false")
                  << ",\"jitter_faststart_min_packets\":" << (options_.expect_anchors ? 2 : 0)
                  << ",\"adaptive_correction\":" << (options_.correct_desktop ? "true":"false")
                  << ",\"clock_epoch\":" << clock_epoch_ << ','
                  << "\"packets_accepted\":" << accepted_.load() << ",\"packets_rejected\":" << rejected_.load()
                  << ",\"invalid_sender_reports\":" << invalid_reports_.load()
                  << ",\"invalid_ingress_anchor_packets\":" << invalid_ingress_anchors_.load()
                  << ",\"sender_session_pinned\":" << (options_.expected_sender_session ? "true" : "false")
                  << ",\"foreign_identity_rtp_packets\":" << foreign_identity_rtp_.load()
                  << ",\"unadmitted_rtcp_packets\":" << unadmitted_rtcp_.load()
                  << ",\"clock_pause_fixture\":" << (clock_paused ? "true" : "false")
                  << ",\"clock_resumed\":" << (clock_resumed ? "true" : "false")
                  << ",\"warnings\":" << warnings_
                  << ",\"jitter_drop_messages\":" << jitter_drop_messages_
                  << ",\"jitter_num_too_late\":" << jitter_num_too_late_
                  << ",\"jitter_num_drop_on_latency\":" << jitter_num_drop_on_latency_
                  << ",\"invalid_jitter_drop_messages\":" << invalid_jitter_drop_messages_
                  << ",\"first_jitter_drop_seqnum\":";
        json_optional(first_jitter_drop_seqnum_);
        std::cout << ",\"first_jitter_drop_reason\":";
        json_optional(first_jitter_drop_reason_);
        std::cout << ",\"jitter_drop_reason_codes\":{\"unknown\":0,\"too_late\":1,\"drop_on_latency\":2}"
                  << ",\"sessions\":[";
        for (std::size_t n = 0; n < branches_.size(); ++n) {
            const auto& b = *branches_[n];
            timed += b.timed; buffers += b.buffers;
            invalid += b.invalid_reference + b.stale_reference + b.nonmonotonic + b.untimed_after_lock;
            original_invalid += b.original_invalid;
            if (active_epoch && active_ssrc && b.last_original && b.report &&
                b.last_original->epoch == *active_epoch && b.report->ssrc == *active_ssrc) {
                ++active_branches;
                active_windows = b.original_measurements;
                active_usable_windows = b.original_usable_measurements;
                active_last_estimate_within_limit = b.last_original_estimate_within_limit;
            }
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
            // Keep original-clock diagnostics separate from nominal RTCP references.
        }
        std::cout << "],\"original_anchor_ingress\":[";
        {
            std::lock_guard lock(report_mutex_);
            bool comma = false;
            for (const auto& slot : reports_) {
                if (!slot.occupied) continue;
                if (comma) std::cout << ',';
                comma = true;
                const auto packets = slot.ingress_rtp_packets.load();
                std::cout << "{\"ssrc\":" << slot.ssrc << ",\"rtp_packets\":" << packets
                    << ",\"rtp_frames\":" << slot.ingress_rtp_frames.load()
                    << ",\"first_wire_start\":";
                if (packets) std::cout << slot.ingress_first_wire_start.load(); else std::cout << "null";
                std::cout << ",\"last_wire_start\":";
                if (packets) std::cout << slot.ingress_last_wire_start.load(); else std::cout << "null";
                std::cout << '}';
            }
        }
        std::cout << "],\"original_anchor_sessions\":[";
        for (std::size_t n = 0; n < branches_.size(); ++n) {
            const auto& b = *branches_[n];
            if (n) std::cout << ',';
            std::cout << "{\"ssrc\":" << (b.report ? b.report->ssrc : 0)
                << ",\"callbacks\":" << b.original_callbacks
                << ",\"first_callback_wire_start\":";
            json_optional(b.first_callback_wire_start);
            std::cout << ",\"first_callback_anchor_sequence\":";
            json_optional(b.first_callback_anchor_sequence);
            std::cout << ",\"first_failure_status\":";
            json_optional(b.first_original_failure_status);
            std::cout << ",\"first_failure_wire_start\":";
            json_optional(b.first_failure_wire_start);
            std::cout << ",\"first_failure_anchor_sequence\":";
            json_optional(b.first_failure_anchor_sequence);
            std::cout << ",\"first_failure_capture_age_ns\":";
            json_optional(b.first_failure_capture_age_ns);
            std::cout << ",\"packets\":" << b.original_packets << ",\"repeated_anchors\":" << b.original_repeats
                << ",\"measurements\":" << b.original_measurements << ",\"out_of_range\":" << b.original_out_of_range
                << ",\"usable_measurements\":" << b.original_usable_measurements
                << ",\"last_estimate_within_limit\":" << (!b.original_measurements ? "null" :
                    b.last_original_estimate_within_limit ? "true" : "false")
                << ",\"invalid\":" << b.original_invalid << ",\"retired_or_foreign\":" << b.original_retired
                << ",\"frames\":" << b.original_frames << ",\"last_status\":" << b.last_original_status
                << ",\"last_rate_ppm\":" << b.last_original_ppm;
            if (b.last_original) std::cout << ",\"generation\":" << b.last_original->epoch.generation
                << ",\"first_device_position\":" << b.first_original->device_position
                << ",\"last_device_position\":" << b.last_original->device_position
                << ",\"first_capture_ns\":" << b.first_original->capture_ns
                << ",\"last_capture_ns\":" << b.last_original->capture_ns
                << ",\"last_qpc_100ns\":" << b.last_original->qpc_100ns
                << ",\"last_anchor_sequence\":" << b.last_original->anchor_sequence
                << ",\"last_calibration_revision\":" << b.last_original->calibration_revision;
            std::cout << '}';
        }
        // Historical observation only: the finite receiver can intentionally
        // outlive its sender. Never sum acquisition windows across generations,
        // count repeated anchors as new windows, or hide a later bad estimate.
        const bool historical_qualified = active_branches == 1 && !original_invalid &&
            active_usable_windows >= 3 && active_last_estimate_within_limit;
        const bool bad_anchors = options_.expect_anchors && !historical_qualified;
        std::cout << "],\"active_generation_measurements\":" << active_windows
            << ",\"active_generation_usable_measurements\":" << active_usable_windows
            << ",\"active_generation_last_estimate_within_limit\":" << (!active_windows ? "null" :
                active_last_estimate_within_limit ? "true" : "false")
            << ",\"historical_original_anchor_qualification\":" << (historical_qualified ? "true" : "false")
            << ",\"qualification_semantics\":\"historical_active_generation_windows_not_present_lock\"";
        bool bad_correction=false,recovered_with_gap=false,awaiting_generation=false;
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
        if (correction_) {
            std::cout<<",\"correction_queue_peak_packets\":"<<correction_->queue_peak()<<",\"correction_sessions\":[";
            const auto sessions=correction_->sessions();
            bad_correction=!avsync::correction_diagnostic_pass(sessions,correction_->failed(),clock_pause_ns);
            awaiting_generation=options_.recover_desktop_ipc && correction_->awaiting_generation();
            recovered_with_gap=options_.recover_desktop_ipc && !correction_->failed() && sessions.size()>1 &&
                sessions.back().state==avsync::CorrectionState::running &&
                sessions.back().fault==avsync::CorrectionFault::none && sessions.back().delivered_frames>=48000;
            for (std::size_t i=0;i<sessions.size();++i) {
                const auto& s=sessions[i]; const auto& d=s.correction;
                if (i) std::cout<<',';
                std::cout<<"{\"generation\":"<<s.epoch.generation<<",\"ssrc\":"<<s.ssrc
                    <<",\"state\":"<<static_cast<unsigned>(s.state)<<",\"fault\":"<<static_cast<unsigned>(s.fault)
                    <<",\"packets\":"<<s.packets<<",\"received_frames\":"<<d.received_frames
                    <<",\"priming_discarded_frames\":"<<d.priming_discarded_frames
                    <<",\"produced_frames\":"<<d.produced_frames<<",\"delivered_frames\":"<<s.delivered_frames
                    <<",\"command_ppm\":"<<s.command_ppm<<",\"target_ppm\":"<<s.target_ppm
                    <<",\"acquisition_spread_ppm\":"<<d.acquisition_spread_ppm
                    <<",\"maximum_predicted_phase_ns\":"<<d.maximum_predicted_phase_ns
                    <<",\"maximum_output_age_ns\":"<<s.maximum_output_age_ns
                    <<",\"stale_reason\":"<<static_cast<unsigned>(d.stale_reason)
                    <<",\"stale_age_ns\":";
                json_optional(d.stale_age_ns);
                std::cout
                    <<",\"peak_input_frames\":"<<d.peak_input_frames<<",\"peak_output_frames\":"<<d.peak_output_frames
                    <<",\"maximum_dispatch_quanta\":"<<s.dispatch_calls_max
                    <<",\"first_capture_ns\":"<<s.first_capture_ns<<",\"last_capture_ns\":"<<s.last_capture_ns
                    <<",\"output_peak\":"<<s.peak<<",\"output_rms\":"
                    <<(s.delivered_frames ? std::sqrt(static_cast<double>(s.sum_squares/(2*s.delivered_frames))):0)
                    <<",\"first_fault_ns\":";
                json_optional(s.first_fault_ns); std::cout<<'}';
            }
            std::cout<<"],\"correction_state_codes\":{\"priming\":0,\"running\":1,\"faulted\":2}"
                <<",\"correction_fault_codes\":{\"none\":0,\"metadata\":1,\"pcm\":2,\"rate\":3,\"overflow\":4,\"stale\":5,\"health\":6,\"backend\":7,\"timeline\":8,\"phase\":9}"
                <<",\"correction_stale_reason_codes\":{\"none\":0,\"no_progress\":1,\"anchor_age\":2,\"input_queue\":3,\"output_queue\":4}"
                <<",\"corrected_pcm_destination\":\""<<(options_.desktop_ipc.empty()?
                    "in_memory_statistics_then_discard":"private_desktop_ipc_2_seconds")<<"\""
                <<",\"correction_diagnostic_pass\":"<<(bad_correction?"false":"true");
            std::cout<<",\"same_session_recovery_enabled\":"<<(options_.recover_desktop_ipc?"true":"false")
                <<",\"awaiting_media_generation\":"<<(correction_->awaiting_generation()?"true":"false")
                <<",\"retired_correction_packets\":"<<correction_->retired_packets();
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
            if (handoff_) std::cout<<",\"ipc_published_frames\":"<<retired_ipc_frames_+handoff_->published_frames()
                <<",\"ipc_busy_retries\":"<<retired_ipc_busy_+handoff_->busy_retries()
                <<",\"ipc_retry_queue_peak_blocks\":"<<std::max(retired_ipc_peak_,handoff_->queue_peak())
                <<",\"ipc_recreations\":"<<ipc_recreations_
                <<",\"ipc_failure\":\""<<handoff_->failure_reason()<<"\"";
#endif
        }
#endif
        const bool controlled_stop = control_.state() != avsync::process::ControlState::active;
        std::cout << ",\"control_state\":\"" << avsync::process::state_name(control_.state()) << "\""
            << ",\"present_lock_verified\":false,\"status\":\"" << (error || fatal_ ? "error" :
            controlled_stop ? avsync::process::state_name(control_.state()) :
            awaiting_generation ? "awaiting_media_generation" :
            recovered_with_gap && !bad_anchors && !invalid ? "recovered_with_gap" :
            bad_correction ? "correction_unqualified" : bad_anchors ? "original_anchors_unqualified" :
            !buffers ? "waiting_media" : !timed ? "priming_reference" : invalid ? "invalid_timing" :
            options_.expect_anchors ? "original_anchors_observed" : "timestamps_observed") << "\"}\n";
        if (error || fatal_) return 1;
        // STOP acknowledges control cleanup only; qualification/fault fields above
        // remain independent. EOF/lease expiry/invalid input are failed control.
        if (controlled_stop) return control_.state() == avsync::process::ControlState::stopped ? 0 : 1;
        return bad_correction || bad_anchors || (options_.expect_media && (!timed || invalid)) ? 3 : 0;
    }
private:
    int early_control_exit() {
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
        if (handoff_) handoff_->revoke();
#endif
        std::cout << "{\"schema\":1,\"status\":\"" << avsync::process::state_name(control_.state())
                  << "\",\"capture_timing_verified\":false}\n";
        return control_.state() == avsync::process::ControlState::stopped ? 0 : 1;
    }
    static void new_jitterbuffer(GstElement*, GstElement* jitterbuffer,
            guint session, guint, gpointer opaque) {
        if (session != 0) return;
        const auto* self = static_cast<Receiver*>(opaque);
        // Waiting the full latency at startup can fill the media-span bound
        // before its release timer fires, dropping frame zero on bursty input.
        // Start after two consecutive packets; retain the 100ms loss/reorder
        // budget and drop-on-latency cap. This is not presentation scheduling.
        if (self->options_.expect_anchors)
            g_object_set(jitterbuffer, "faststart-min-packets", 2u, nullptr);
        g_object_set(jitterbuffer, "post-drop-messages", TRUE,
            "drop-messages-interval", 0u, nullptr);
    }
    void observe_drop_message(GstMessage* message) noexcept {
        const auto* fields = gst_message_get_structure(message);
        if (!fields || !gst_structure_has_name(fields, "drop-msg")) return;
        guint sequence{}, too_late{}, on_latency{};
        if (!gst_structure_get_uint(fields, "seqnum", &sequence) || sequence > 65535 ||
            !gst_structure_get_uint(fields, "num-too-late", &too_late) ||
            !gst_structure_get_uint(fields, "num-drop-on-latency", &on_latency)) {
            ++invalid_jitter_drop_messages_;
            return;
        }
        ++jitter_drop_messages_;
        jitter_num_too_late_ += too_late;
        jitter_num_drop_on_latency_ += on_latency;
        if (first_jitter_drop_seqnum_) return;
        first_jitter_drop_seqnum_ = sequence;
        const auto* reason = gst_structure_get_string(fields, "reason");
        // Only publish stable numeric codes, never arbitrary bus text.
        first_jitter_drop_reason_ = !reason ? 0u :
            std::string_view(reason) == "too-late" ? 1u :
            std::string_view(reason) == "drop-on-latency" ? 2u : 0u;
    }
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
        if (options_.expect_anchors && std::string_view(requested) == "recv_rtp_sink_0")
            gst_pad_add_probe(src, GST_PAD_PROBE_TYPE_BUFFER, observe_rtp, this, nullptr);
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
    static GstPadProbeReturn observe_rtp(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto* self = static_cast<Receiver*>(opaque);
        if (self->pinned_ingress_) {
            avsync::net::PinnedIngressResult result;
            {
                std::lock_guard lock(self->report_mutex_);
                result = self->pinned_ingress_->observe_rtp(GST_PAD_PROBE_INFO_BUFFER(info));
            }
            if (result == avsync::net::PinnedIngressResult::foreign_identity) {
                ++self->foreign_identity_rtp_; return GST_PAD_PROBE_DROP;
            }
            if (result != avsync::net::PinnedIngressResult::accepted) {
                ++self->invalid_ingress_anchors_; self->fatal_.store(true);
                return GST_PAD_PROBE_DROP;
            }
        }
        std::uint32_t ssrc{}, timestamp{}, frames{};
        const auto record = avsync::net::read_audio_anchor(
            GST_PAD_PROBE_INFO_BUFFER(info), ssrc, timestamp, frames);
        if (!record) {
            ++self->invalid_ingress_anchors_;
            return GST_PAD_PROBE_OK; // Observability only; ordered validation still decides.
        }
        try {
            if (auto* slot = self->report_slot(ssrc)) {
                if (slot->ingress_rtp_packets.fetch_add(1) == 0)
                    slot->ingress_first_wire_start.store(record->packet_wire_start);
                slot->ingress_last_wire_start.store(record->packet_wire_start);
                slot->ingress_rtp_frames.fetch_add(frames);
            }
        } catch (...) { self->fatal_.store(true); }
        return GST_PAD_PROBE_OK;
    }
    static GstPadProbeReturn observe_rtcp(GstPad*, GstPadProbeInfo* info, gpointer opaque) {
        auto* self = static_cast<Receiver*>(opaque);
        auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buffer || !gst_rtcp_buffer_validate_reduced(buffer)) {
            ++self->invalid_reports_; return GST_PAD_PROBE_DROP;
        }
        if (self->pinned_ingress_) {
            std::lock_guard lock(self->report_mutex_);
            if (!self->pinned_ingress_->allows_rtcp(buffer)) {
                ++self->unadmitted_rtcp_; return GST_PAD_PROBE_DROP;
            }
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
            state->owner = self;
            if (self->options_.expect_anchors) state->validator.emplace(self->clock_epoch_);
            const auto suffix = name.substr(std::string_view("recv_rtp_src_0_").size());
            const auto separator = suffix.find('_');
            guint32 ssrc{};
            require(separator != suffix.npos, "Invalid session pad name");
            const auto parsed = std::from_chars(suffix.data(), suffix.data()+separator, ssrc);
            require(parsed.ec == std::errc{} && parsed.ptr == suffix.data()+separator, "Invalid session identifier");
            state->report = self->report_slot(ssrc);
            require(state->report != nullptr, "Report session limit reached");
            auto* depay = self->options_.expect_anchors ? nullptr : self->add("rtpL24depay", nullptr);
            auto* sink = self->add("appsink", nullptr);
            g_object_set(sink, "sync", FALSE, "async", FALSE, "emit-signals", TRUE,
                         "max-buffers", 32u, "drop", TRUE, "enable-last-sample", FALSE, nullptr);
            if (depay) {
                auto* raw_caps = gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S24BE",
                    "rate", G_TYPE_INT, 48000, "channels", G_TYPE_INT, 2, "layout", G_TYPE_STRING, "interleaved", nullptr);
                gst_app_sink_set_caps(GST_APP_SINK(sink), raw_caps);
                gst_caps_unref(raw_caps);
            }
            // Retain callback state even if a later linking/state operation fails.
            self->branches_.push_back(std::move(branch));
            g_signal_connect(sink, "new-sample", G_CALLBACK(new_sample), state);
            if (depay) require(gst_element_link(depay, sink), "Cannot link audio depayloader");
            auto* depay_sink = gst_element_get_static_pad(depay ? depay : sink, "sink");
            const auto linked = gst_pad_link(pad, depay_sink);
            gst_object_unref(depay_sink);
            require(linked == GST_PAD_LINK_OK, "Cannot connect received session");
            require(gst_element_sync_state_with_parent(sink) && (!depay || gst_element_sync_state_with_parent(depay)),
                    "Cannot activate received session");
        } catch (...) { self->fatal_.store(true); }
    }
    static GstFlowReturn new_sample(GstAppSink* sink, gpointer opaque) {
        auto& b = *static_cast<Branch*>(opaque);
        auto* sample = gst_app_sink_pull_sample(sink);
        if (!sample) return GST_FLOW_EOS;
        auto* buffer = gst_sample_get_buffer(sample);
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        GstMapInfo map{};
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
        std::optional<avsync::DiagnosticAudioPacket> correction_packet;
#endif
        if (b.validator) {
            std::uint32_t ssrc{}, timestamp{}, payload_frames{};
            const auto record = avsync::net::read_audio_anchor(buffer, ssrc, timestamp, payload_frames);
            const auto now = gst_util_get_timestamp();
            if (b.owner->options_.recover_desktop_ipc && record && b.owner->retired_record(*record)) {
                ++b.original_retired; gst_sample_unref(sample); return GST_FLOW_OK;
            }
            if (b.original_callbacks++ == 0 && record) {
                b.first_callback_wire_start = record->packet_wire_start;
                b.first_callback_anchor_sequence = record->anchor_sequence;
            }
            if (!record || now > static_cast<guint64>(std::numeric_limits<std::int64_t>::max())) {
                const auto rejected = b.validator->reject_missing();
                b.last_original_status = static_cast<unsigned>(rejected.status);
                remember_original_failure(b, rejected.status, record, now);
                ++b.original_invalid;
                if (b.owner->options_.recover_desktop_ipc) b.owner->fatal_.store(true);
                gst_sample_unref(sample); return GST_FLOW_OK;
            }
            const bool expiry_only=b.owner->options_.recover_desktop_ipc && b.validator->timing_only_stale(
                *record,ssrc,timestamp,payload_frames,static_cast<avsync::Nanoseconds>(now));
            const auto validation = b.validator->observe(*record, ssrc, timestamp, payload_frames,
                static_cast<std::int64_t>(now));
            b.last_original_status = static_cast<unsigned>(validation.status);
            if (!validation.accepted) {
                remember_original_failure(b, validation.status, record, now);
                bool retired=b.owner->options_.recover_desktop_ipc && b.owner->retired_record(*record);
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
                if (!retired && expiry_only && b.owner->correction_)
                    retired=b.owner->correction_->retire_stale_transport(record->epoch);
#else
                (void)expiry_only;
#endif
                if (retired) ++b.original_retired;
                else {
                    ++b.original_invalid;
                    if (b.owner->options_.recover_desktop_ipc) b.owner->fatal_.store(true);
                }
                gst_sample_unref(sample); return GST_FLOW_OK;
            }
            bool admitted{};
            try {
                std::lock_guard lock(b.owner->admission_mutex_);
                admitted = b.owner->admission_->admit(*record, ssrc);
            } catch (...) {
                b.owner->fatal_.store(true);
                gst_sample_unref(sample); return GST_FLOW_ERROR;
            }
            if (!admitted) {
                ++b.original_retired;
                if (b.owner->options_.recover_desktop_ipc && !b.owner->retired_record(*record)) b.owner->fatal_.store(true);
                gst_sample_unref(sample); return GST_FLOW_OK;
            }
            ++b.original_packets;
            b.original_frames += payload_frames;
            if (validation.repeated) ++b.original_repeats;
            if (validation.estimate) {
                ++b.original_measurements;
                b.last_original_ppm = validation.estimate->source_rate_error_ppm;
                b.last_original_estimate_within_limit = validation.estimate->within_correction_limit;
                if (b.last_original_estimate_within_limit) ++b.original_usable_measurements;
                else {
                    ++b.original_out_of_range;
                    // Latch before queuing: a later retirement request must not
                    // overtake and discard a still-pending terminal rate fault.
                    if (b.owner->options_.recover_desktop_ipc) {
                        b.owner->fatal_.store(true); gst_sample_unref(sample); return GST_FLOW_OK;
                    }
                }
            }
            if (!b.first_original) b.first_original = record;
            b.last_original = record;
            if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) { gst_sample_unref(sample); return GST_FLOW_ERROR; }
            map.data = static_cast<guint8*>(gst_rtp_buffer_get_payload(&rtp));
            map.size = gst_rtp_buffer_get_payload_len(&rtp);
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
            if (b.owner->correction_) {
                correction_packet.emplace();
                correction_packet->record=*record; correction_packet->ssrc=ssrc;
                correction_packet->timestamp=timestamp; correction_packet->frames=payload_frames;
                correction_packet->arrival_ns=static_cast<avsync::Nanoseconds>(now);
                const auto sr=b.report ? b.report->last_received.load():GST_CLOCK_TIME_NONE;
                if (GST_CLOCK_TIME_IS_VALID(sr) && sr<=static_cast<guint64>(std::numeric_limits<std::int64_t>::max()))
                    correction_packet->last_report_ns=static_cast<avsync::Nanoseconds>(sr);
            }
#endif
        } else if (!buffer || !gst_buffer_map(buffer, &map, GST_MAP_READ)) {
            gst_sample_unref(sample); return GST_FLOW_ERROR;
        }
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
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
        if (correction_packet && (!avsync::decode_l24(
                std::span(reinterpret_cast<const std::byte*>(map.data),map.size),*correction_packet) ||
                !b.owner->correction_->submit(*correction_packet))) b.owner->fatal_.store(true);
#endif
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
        if (b.validator) gst_rtp_buffer_unmap(&rtp);
        else gst_buffer_unmap(buffer,&map);
        gst_sample_unref(sample);
        return GST_FLOW_OK;
    }
    bool retired_record(const avsync::wire::AudioRecord& record) noexcept {
        try {
            std::lock_guard lock(admission_mutex_);
            const auto active=admission_->active_epoch();
            if (active && record.epoch.session==active->session && record.epoch.generation<active->generation) return true;
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
            return options_.recover_desktop_ipc && correction_ && correction_->retired_generation(record.epoch);
#else
            return false;
#endif
        } catch (...) { fatal_.store(true); return false; }
    }
    Options options_;
    avsync::process::StdinControl control_;
    Inet peer_;
    GstElement *pipeline_{}, *rtpbin_{};
    GstClock* clock_{};
    GstNetTimeProvider* provider_{};
    std::mutex branch_mutex_;
    std::mutex report_mutex_;
    std::mutex admission_mutex_;
    std::optional<avsync::wire::AudioStreamAdmission> admission_;
    std::optional<avsync::net::PinnedAudioIngress> pinned_ingress_;
    std::uint64_t clock_epoch_{};
    std::array<ReportSlot, 8> reports_;
    std::vector<std::unique_ptr<Branch>> branches_;
    std::atomic<bool> fatal_{false};
    std::atomic<std::uint64_t> accepted_{0}, rejected_{0};
    std::atomic<std::uint64_t> invalid_reports_{0};
    std::atomic<std::uint64_t> invalid_ingress_anchors_{0};
    std::atomic<std::uint64_t> foreign_identity_rtp_{0}, unadmitted_rtcp_{0};
    std::uint64_t warnings_{};
    // Main bus-loop owner; constant storage for this finite diagnostic run.
    std::uint64_t jitter_drop_messages_{}, jitter_num_too_late_{}, jitter_num_drop_on_latency_{};
    std::uint64_t invalid_jitter_drop_messages_{};
    std::optional<unsigned> first_jitter_drop_seqnum_, first_jitter_drop_reason_;
#ifdef AVSYNC_HAS_AUDIO_HANDOFF
    std::unique_ptr<avsync::CorrectedAudioHandoff> handoff_;
    std::uint64_t retired_ipc_frames_{},retired_ipc_busy_{},ipc_recreations_{};
    std::size_t retired_ipc_peak_{};
#endif
#ifdef AVSYNC_HAS_AUDIO_CORRECTION
    std::unique_ptr<avsync::AudioCorrectionDiagnostic> correction_;
#endif
};
}
int main(int argc,char** argv) {
    if (argc==1 || (argc==2 && std::string_view(argv[1])=="--help")) { help(); return 0; }
    try {
        Options options;
        for (int i=1;i<argc;++i) {
            const std::string_view arg(argv[i]);
            if(arg=="--expect-media") { options.expect_media=true; continue; }
            if(arg=="--expect-anchors") { options.expect_anchors=true; continue; }
            if(arg=="--correct-desktop") { options.correct_desktop=true; options.expect_anchors=true; continue; }
            if(arg=="--recover-desktop-ipc") { options.recover_desktop_ipc=true; continue; }
            if(arg=="--control-stdin") {
                require(!options.control_stdin,"Duplicate control option"); options.control_stdin=true; continue;
            }
            if(arg=="--replace-desktop-ipc") {
                require(!options.replace_desktop_ipc,"Duplicate replacement option"); options.replace_desktop_ipc=true; continue;
            }
            require(i+1<argc,"Missing option value");
            const auto* value=argv[++i];
            if(arg=="--bind") options.bind=value;
            else if(arg=="--peer") options.peer=value;
            else if(arg=="--clock-port") options.clock_port=number(value,1024,65535);
            else if(arg=="--rtp-port") options.rtp_port=number(value,1024,65535);
            else if(arg=="--rtcp-port") options.rtcp_port=number(value,1024,65535);
            else if(arg=="--seconds") options.seconds=number(value,1,180);
            else if(arg=="--expect-sender-session") {
                require(!options.expected_sender_session, "Duplicate expected sender session");
                options.expected_sender_session=identity(value);
            }
            else if(arg=="--desktop-ipc") { options.desktop_ipc=value; options.correct_desktop=true; options.expect_anchors=true; }
            else if(arg=="--clock-pause-after") options.clock_pause_after=number(value,1,179);
            else if(arg=="--clock-pause-seconds") options.clock_pause_seconds=number(value,1,10);
            else throw std::runtime_error("Unknown option");
        }
        require(!options.bind.empty() && !options.peer.empty() && options.clock_port && options.rtp_port && options.rtcp_port,
                "Explicit bind, peer and three ports are required");
        require(!options.expected_sender_session || options.expect_anchors,
                "Pinned sender identity requires original-anchor mode");
        require(!options.control_stdin || options.expected_sender_session,
                "Control stdin requires an explicit pinned sender identity");
        require(!options.replace_desktop_ipc || (options.control_stdin && !options.desktop_ipc.empty()),
                "IPC replacement requires explicit desktop output and pinned stdin control");
        gst_init(nullptr,nullptr);
        Receiver receiver(options);
        return receiver.run();
    } catch(const std::exception& error) { std::cerr<<error.what()<<'\n'; return 1; }
}
