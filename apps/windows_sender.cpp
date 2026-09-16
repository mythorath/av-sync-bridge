// SPDX-License-Identifier: GPL-2.0-or-later
// Experimental desktop-only sender. No startup installation or OBS changes.
#ifndef _WIN32
#error "The WASAPI sender is Windows-only"
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <audioclient.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <wrl/client.h>

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/audio/audio.h>
#include <gst/net/gstnet.h>
#include <gst/rtp/gstrtpbuffer.h>
#include <gst/rtp/gstrtcpbuffer.h>
#include <avsync/network_clock.hpp>
#include <avsync/capture_window.hpp>
#include <avsync/audio_anchors.hpp>
#include <avsync/nominal_audio.hpp>
#include <avsync/rtp_audio_anchor.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {
using Microsoft::WRL::ComPtr;
using Steady = std::chrono::steady_clock;
constexpr auto max_age_ns = std::int64_t{100'000'000};
constexpr avsync::CaptureWindowPolicy capture_window{100'000'000, max_age_ns};
constexpr unsigned max_resets = 8;
constexpr guint payload_type = 96;

struct Failure { const char *stage; std::uint32_t code = 0; };
void check(HRESULT hr, const char *stage)
{
    if (FAILED(hr)) throw Failure{stage, static_cast<std::uint32_t>(hr)};
}
void require(bool value, const char *stage) { if (!value) throw Failure{stage}; }

struct GstObjectDeleter {
    template<class T> void operator()(T *p) const { if (p) gst_object_unref(p); }
};
template<class T> using GstPtr = std::unique_ptr<T, GstObjectDeleter>;
struct PipelineDeleter {
    void operator()(GstElement *p) const
    {
        if (p) { gst_element_set_state(p, GST_STATE_NULL); gst_object_unref(p); }
    }
};
struct CapsDeleter { void operator()(GstCaps *p) const { if (p) gst_caps_unref(p); } };
using CapsPtr = std::unique_ptr<GstCaps, CapsDeleter>;
struct BufferDeleter { void operator()(GstBuffer *p) const { if (p) gst_buffer_unref(p); } };
using BufferPtr = std::unique_ptr<GstBuffer, BufferDeleter>;

class ComScope {
public:
    ComScope() { check(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED), "com_initialize"); }
    ~ComScope() { CoUninitialize(); }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
};
struct FormatDeleter { void operator()(WAVEFORMATEX *p) const { CoTaskMemFree(p); } };

struct Arguments {
    std::string host;
    unsigned clock_port{}, rtp_port{}, rtcp_port{}, seconds{};
    std::uint64_t clock_epoch{};
};

bool unsigned_value(std::string_view text, unsigned &value)
{
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    return parsed.ec == std::errc{} && parsed.ptr == text.data() + text.size();
}
bool unicast_ipv4(std::string_view host)
{
    std::array<unsigned, 4> parts{};
    for (unsigned i = 0; i < parts.size(); ++i) {
        const auto dot = host.find('.');
        if ((i < 3 && dot == host.npos) || (i == 3 && dot != host.npos)) return false;
        const auto part = dot == host.npos ? host : host.substr(0, dot);
        if (!unsigned_value(part, parts[i]) || parts[i] > 255) return false;
        if (dot != host.npos) host.remove_prefix(dot + 1);
    }
    return parts[0] != 0 && parts[0] < 224;
}
std::optional<Arguments> parse_arguments(int argc, char **argv)
{
    Arguments result;
    bool loopback = false;
    unsigned seen = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--loopback" && !loopback) { loopback = true; continue; }
        if (i + 1 >= argc) return std::nullopt;
        const std::string_view value(argv[++i]);
        unsigned bit = 0;
        unsigned *number = nullptr;
        if (arg == "--host") bit = 1;
        else if (arg == "--clock-port") { bit = 2; number = &result.clock_port; }
        else if (arg == "--rtp-port") { bit = 4; number = &result.rtp_port; }
        else if (arg == "--rtcp-port") { bit = 8; number = &result.rtcp_port; }
        else if (arg == "--seconds") { bit = 16; number = &result.seconds; }
        else if (arg == "--clock-epoch") bit = 32;
        else return std::nullopt;
        if (seen & bit) return std::nullopt;
        seen |= bit;
        if (number) {
            if (!unsigned_value(value, *number)) return std::nullopt;
        } else if (bit == 32) {
            const auto parsed = std::from_chars(value.data(), value.data()+value.size(), result.clock_epoch);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data()+value.size() || !result.clock_epoch)
                return std::nullopt;
        } else result.host = value;
    }
    if (!loopback || seen != 63 || !unicast_ipv4(result.host) || result.seconds < 1 || result.seconds > 120)
        return std::nullopt;
    const auto port = [](unsigned p) { return p >= 1 && p <= 65535; };
    if (!port(result.clock_port) || !port(result.rtp_port) || !port(result.rtcp_port) ||
        result.clock_port == result.rtp_port || result.clock_port == result.rtcp_port ||
        result.rtp_port == result.rtcp_port) return std::nullopt;
    return result;
}

struct CaptureFormat {
    GstAudioInfo info{};
    std::uint32_t windows_mask{};
    std::array<std::array<float, 8>, 2> mix{};
    unsigned channels{}, rate{}, block_align{};
    bool unsigned_eight_bit{};
};

CaptureFormat describe_format(const WAVEFORMATEX &wave)
{
    require(wave.nChannels >= 1 && wave.nChannels <= 8 && wave.nSamplesPerSec >= 8000 &&
            wave.nSamplesPerSec <= 384000, "unsupported_capture_dimensions");
    auto tag = wave.wFormatTag;
    auto valid_bits = wave.wBitsPerSample;
    std::uint32_t mask = 0;
    if (tag == WAVE_FORMAT_EXTENSIBLE) {
        require(wave.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX), "invalid_extensible_format");
        const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave);
        constexpr GUID pcm{WAVE_FORMAT_PCM, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        constexpr GUID ieee{WAVE_FORMAT_IEEE_FLOAT, 0, 0x0010, {0x80, 0, 0, 0xaa, 0, 0x38, 0x9b, 0x71}};
        if (IsEqualGUID(extended.SubFormat, pcm)) tag = WAVE_FORMAT_PCM;
        else if (IsEqualGUID(extended.SubFormat, ieee)) tag = WAVE_FORMAT_IEEE_FLOAT;
        else throw Failure{"unsupported_capture_subformat"};
        valid_bits = extended.Samples.wValidBitsPerSample;
        mask = extended.dwChannelMask;
    }
    // Left-justified valid bits in a larger PCM container require a separate,
    // tested conversion contract. Do not silently treat that format as S32LE.
    require(valid_bits == wave.wBitsPerSample, "unsupported_valid_bit_layout");
    GstAudioFormat sample_format = GST_AUDIO_FORMAT_UNKNOWN;
    if (tag == WAVE_FORMAT_IEEE_FLOAT) {
        if (wave.wBitsPerSample == 32) sample_format = GST_AUDIO_FORMAT_F32LE;
        if (wave.wBitsPerSample == 64) sample_format = GST_AUDIO_FORMAT_F64LE;
    } else if (tag == WAVE_FORMAT_PCM) {
        if (wave.wBitsPerSample == 8) sample_format = GST_AUDIO_FORMAT_U8;
        if (wave.wBitsPerSample == 16) sample_format = GST_AUDIO_FORMAT_S16LE;
        if (wave.wBitsPerSample == 24) sample_format = GST_AUDIO_FORMAT_S24LE;
        if (wave.wBitsPerSample == 32) sample_format = GST_AUDIO_FORMAT_S32LE;
    }
    require(sample_format != GST_AUDIO_FORMAT_UNKNOWN && wave.nBlockAlign ==
            wave.nChannels * (wave.wBitsPerSample / 8), "unsupported_capture_sample_format");
    CaptureFormat result;
    result.windows_mask = mask;
    result.channels = wave.nChannels;
    result.rate = wave.nSamplesPerSec;
    result.block_align = wave.nBlockAlign;
    result.unsigned_eight_bit = sample_format == GST_AUDIO_FORMAT_U8;
    std::array<GstAudioChannelPosition, 8> positions{};
    if (wave.nChannels == 1 && (mask == 0 || mask == 4)) {
        positions[0] = GST_AUDIO_CHANNEL_POSITION_MONO;
        result.mix[0][0] = result.mix[1][0] = 1.0f;
    } else {
        if (mask == 0 && wave.nChannels == 2) mask = 3;
        constexpr std::uint32_t accepted_mask = 0x73f; // FL FR FC LFE BL BR BC SL SR
        require((mask & ~accepted_mask) == 0 && std::popcount(mask) == wave.nChannels,
                "unsupported_capture_channel_mask");
        unsigned column = 0;
        constexpr float k = 0.7071067811865475f;
        for (unsigned bit = 0; bit <= 10; ++bit) {
            if (!(mask & (1u << bit))) continue;
            auto &left = result.mix[0][column];
            auto &right = result.mix[1][column];
            switch (bit) {
            case 0: positions[column] = GST_AUDIO_CHANNEL_POSITION_FRONT_LEFT; left = 1; break;
            case 1: positions[column] = GST_AUDIO_CHANNEL_POSITION_FRONT_RIGHT; right = 1; break;
            case 2: positions[column] = GST_AUDIO_CHANNEL_POSITION_FRONT_CENTER; left = right = k; break;
            case 3: positions[column] = GST_AUDIO_CHANNEL_POSITION_LFE1; break;
            case 4: positions[column] = GST_AUDIO_CHANNEL_POSITION_REAR_LEFT; left = k; break;
            case 5: positions[column] = GST_AUDIO_CHANNEL_POSITION_REAR_RIGHT; right = k; break;
            case 8: positions[column] = GST_AUDIO_CHANNEL_POSITION_REAR_CENTER; left = right = 0.5f; break;
            case 9: positions[column] = GST_AUDIO_CHANNEL_POSITION_SIDE_LEFT; left = k; break;
            case 10: positions[column] = GST_AUDIO_CHANNEL_POSITION_SIDE_RIGHT; right = k; break;
            default: throw Failure{"unsupported_capture_speaker"};
            }
            ++column;
        }
    }
    float maximum_row_sum = 0;
    for (const auto &row : result.mix) {
        float sum = 0;
        for (unsigned i = 0; i < result.channels; ++i) sum += std::abs(row[i]);
        maximum_row_sum = std::max(maximum_row_sum, sum);
    }
    require(maximum_row_sum > 0, "empty_downmix");
    const auto gain = 0.9f / maximum_row_sum;
    for (auto &row : result.mix)
        for (auto &coefficient : row) coefficient *= gain;
    gst_audio_info_init(&result.info);
    gst_audio_info_set_format(&result.info, sample_format, static_cast<gint>(result.rate),
                              static_cast<gint>(result.channels), positions.data());
    require(GST_AUDIO_INFO_IS_VALID(&result.info), "invalid_gstreamer_audio_info");
    return result;
}

struct Packet {
    std::uint64_t device_position{}, qpc_100ns{};
    std::uint32_t frames{};
    DWORD flags{};
    std::size_t bytes{};
};

class Capture {
public:
    Capture()
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                               IID_PPV_ARGS(enumerator.GetAddressOf())), "create_device_enumerator");
        ComPtr<IMMDevice> endpoint;
        check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.GetAddressOf()), "default_render_endpoint");
        check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                 reinterpret_cast<void **>(audio_.GetAddressOf())), "activate_audio_client");
        WAVEFORMATEX *raw = nullptr;
        check(audio_->GetMixFormat(&raw), "get_mix_format");
        std::unique_ptr<WAVEFORMATEX, FormatDeleter> wave(raw);
        require(wave != nullptr, "missing_mix_format");
        format_ = describe_format(*wave);
        check(audio_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                                  1'000'000, 0, wave.get(), nullptr), "initialize_loopback");
        UINT32 buffer_frames = 0;
        check(audio_->GetBufferSize(&buffer_frames), "capture_buffer_size");
        require(buffer_frames > 0 && buffer_frames <= format_.rate / 2, "capture_buffer_too_large");
        scratch_.resize(static_cast<std::size_t>(buffer_frames) * format_.block_align);
        check(audio_->GetService(IID_PPV_ARGS(capture_.GetAddressOf())), "capture_service");
    }
    ~Capture() { if (started_) audio_->Stop(); }
    void start() { check(audio_->Start(), "start_capture"); started_ = true; }
    void stop()
    {
        if (started_) { started_ = false; check(audio_->Stop(), "stop_capture"); }
    }
    bool next(Packet &packet)
    {
        BYTE *pcm = nullptr;
        const auto hr = capture_->GetBuffer(&pcm, &packet.frames, &packet.flags,
                                            &packet.device_position, &packet.qpc_100ns);
        if (hr == AUDCLNT_S_BUFFER_EMPTY) return false;
        check(hr, "capture_get_buffer");
        if (!packet.frames) throw Failure{"empty_success_packet"};
        packet.bytes = static_cast<std::size_t>(packet.frames) * format_.block_align;
        const bool fits = packet.bytes <= scratch_.size();
        const bool silent = (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
        const bool bad_time = (packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) != 0;
        if (fits && !bad_time && (silent || pcm)) {
            if (silent) std::memset(scratch_.data(), format_.unsigned_eight_bit ? 0x80 : 0, packet.bytes);
            else std::memcpy(scratch_.data(), pcm, packet.bytes);
        }
        // Release before any GStreamer allocation, clock mapping or network work.
        check(capture_->ReleaseBuffer(packet.frames), "capture_release_buffer");
        require(fits, "oversized_capture_packet");
        require(bad_time || silent || pcm != nullptr, "missing_capture_pcm");
        return true;
    }
    const CaptureFormat &format() const { return format_; }
    const std::uint8_t *data() const { return scratch_.data(); }
private:
    ComPtr<IAudioClient> audio_;
    ComPtr<IAudioCaptureClient> capture_;
    CaptureFormat format_;
    std::vector<std::uint8_t> scratch_;
    bool started_ = false;
};

struct Rejection {
    const char *reason = "none";
    std::uint64_t device_position{}, qpc_100ns{}, network_now{}, raw_internal_now{};
    std::optional<std::int64_t> qpc_ns, mapped_ns;
    std::optional<avsync::net::Calibration> calibration_snapshot;
};

struct Statistics {
    struct ClockLoss { avsync::net::ClockHealth health; std::uint64_t elapsed_ms{}, mapped_packets{}; };
    std::array<ClockLoss,8> first_clock_losses{};
    std::uint64_t clock_losses{};
    std::uint64_t clock_poll_timeout_ns{};
    std::uint64_t captured_packets{}, captured_frames{}, silent_packets{}, initial_discontinuities{};
    std::uint64_t discontinuities{}, timestamp_errors{}, dropped_packets{}, resets{}, queue_overflows{};
    std::uint64_t mapped_packets{}, first_device_position{}, last_device_position{};
    std::uint64_t first_qpc_100ns{}, last_qpc_100ns{};
    std::int64_t first_capture_ns{}, last_capture_ns{}, domain_bracket_ns{};
    std::uint64_t reject_mapping{}, reject_clock_now{}, reject_future{}, reject_stale{}, reject_nonmonotonic{};
    std::uint64_t reject_unhealthy{}, reject_after_build{}, reject_after_conversion{};
    std::uint64_t converted_input_frames{}, converted_output_frames{}, converter_latency_input_frames{};
    std::uint64_t anchor_observations{}, anchor_measurements{}, anchor_out_of_range{};
    double last_original_rate_ppm{};
    std::int64_t maximum_conversion_ns{};
    std::optional<std::int64_t> minimum_mapped_minus_now, maximum_mapped_minus_now;
    std::optional<std::int64_t> minimum_qpc_minus_internal, maximum_qpc_minus_internal;
    std::array<Rejection, 8> first_rejections{};
    std::size_t rejection_count{};
    std::optional<Rejection> last_rejection;
    guint32 last_ssrc{};
    std::atomic<std::uint64_t> rtp_packets{}, rtcp_packets{}, sender_reports{}, rtp_payload_bytes{};
    std::atomic<std::uint64_t> first_rtp_pts{}, last_rtp_pts{}, last_sr_ntp64{};
    std::atomic<guint32> last_rtp_timestamp{}, last_sr_rtp_timestamp{};
    std::atomic<std::uint64_t> rtp_frame_steps{}, rtp_nominal_pts_steps{}, rtp_invalid_payload{};
    std::atomic<std::uint64_t> anchored_rtp_packets{}, anchor_transport_errors{};
    std::uint64_t selected_anchors{}, calibration_revision{};
    std::uint64_t last_selected_device_position{}, last_selected_sequence{}, last_selected_qpc_100ns{};
    std::int64_t last_selected_capture_ns{};
};

void range_observation(std::optional<std::int64_t> &minimum, std::optional<std::int64_t> &maximum,
                       std::int64_t value)
{
    minimum = minimum ? std::min(*minimum, value) : value;
    maximum = maximum ? std::max(*maximum, value) : value;
}

void record_rejection(Statistics &stats, const char *reason, const Packet &packet,
                      const std::optional<std::int64_t> &mapped, GstClockTime now,
                      GstClock *clock, GstClock *internal)
{
    Rejection rejected;
    rejected.reason = reason;
    rejected.device_position = packet.device_position;
    rejected.qpc_100ns = packet.qpc_100ns;
    rejected.qpc_ns = avsync::net::qpc_100ns_to_ns(packet.qpc_100ns);
    rejected.mapped_ns = mapped;
    rejected.network_now = now;
    rejected.raw_internal_now = gst_clock_get_internal_time(internal);
    // Diagnostic snapshot taken after map_qpc_100ns; a concurrent calibration
    // update can occur between the actual map and this snapshot.
    rejected.calibration_snapshot = avsync::net::client_calibration(clock);
    if (rejected.qpc_ns && rejected.raw_internal_now <=
            static_cast<guint64>(std::numeric_limits<std::int64_t>::max()))
        range_observation(stats.minimum_qpc_minus_internal, stats.maximum_qpc_minus_internal,
                          *rejected.qpc_ns - static_cast<std::int64_t>(rejected.raw_internal_now));
    if (stats.rejection_count < stats.first_rejections.size())
        stats.first_rejections[stats.rejection_count++] = rejected;
    stats.last_rejection = rejected;
}

class SenderPipeline {
public:
    SenderPipeline(const Arguments &args, const CaptureFormat &format, GstClock *clock, Statistics &stats,
                   std::uint64_t session)
        : stats_(stats), converter_(format.info, format.mix, format.rate / 2),
          converted_(48'000 * 2), epoch_{session, stats.resets + 1}, clock_epoch_(args.clock_epoch), clock_(clock)
    {
        stats_.converter_latency_input_frames = converter_.max_latency_input_frames();
        pipeline_.reset(gst_pipeline_new("avsync-desktop-sender"));
        require(pipeline_ != nullptr, "create_pipeline");
        source_ = make("appsrc", "captured-desktop");
        auto *quantize = make("audioconvert", "pcm24-convert");
        auto *wire_caps = make("capsfilter", "wire-format");
        pay_ = make("rtpL24pay", "pcm24-payload");
        rtpbin_ = make("rtpbin", "rtp-session");
        auto *rtp_sink = make("udpsink", "rtp-output");
        auto *rtcp_sink = make("udpsink", "rtcp-output");
        constexpr guint64 max_bytes = 48'000 * 2 * sizeof(float) / 10;
        g_object_set(source_, "is-live", TRUE, "format", GST_FORMAT_TIME, "do-timestamp", FALSE,
                     "block", FALSE, "max-time", static_cast<guint64>(max_age_ns),
                     "max-bytes", max_bytes, "max-buffers", static_cast<guint64>(32),
                     "leaky-type", GST_APP_LEAKY_TYPE_UPSTREAM, "emit-signals", TRUE,
                     "min-latency", static_cast<gint64>(0), "max-latency", static_cast<gint64>(max_age_ns), nullptr);
        CapsPtr input_caps(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "F32LE",
            "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 2,
            "rate", G_TYPE_INT, 48000, "channel-mask", GST_TYPE_BITMASK,
            static_cast<guint64>(3), nullptr));
        require(input_caps != nullptr, "create_input_caps");
        gst_app_src_set_caps(GST_APP_SRC(source_), input_caps.get());
        g_signal_connect(source_, "enough-data", G_CALLBACK(enough_data), this);
        CapsPtr wire(gst_caps_new_simple("audio/x-raw", "format", G_TYPE_STRING, "S24BE",
            "layout", G_TYPE_STRING, "interleaved", "channels", G_TYPE_INT, 2,
            "rate", G_TYPE_INT, 48000, "channel-mask", GST_TYPE_BITMASK, static_cast<guint64>(3), nullptr));
        g_object_set(wire_caps, "caps", wire.get(), nullptr);
        // Deterministic 24-bit quantization for the first conversion fixtures.
        g_object_set(quantize, "dithering", 0, "noise-shaping", 0, nullptr);
        guint32 ssrc;
        do { ssrc = g_random_int(); } while (ssrc == 0 || ssrc == stats.last_ssrc);
        stats.last_ssrc = ssrc;
        g_object_set(pay_, "pt", payload_type, "ssrc", ssrc, "mtu", static_cast<guint>(1096),
                     "max-ptime", static_cast<gint64>(4 * GST_MSECOND), nullptr);
        g_object_set(rtpbin_, "ntp-time-source", 3, "rtcp-sync-send-time", FALSE, nullptr);
        const auto cname = std::string("avsync-desktop-") + std::to_string(ssrc);
        auto *sdes = gst_structure_new("application/x-rtp-source-sdes", "cname", G_TYPE_STRING,
                                      cname.c_str(), "tool", G_TYPE_STRING, "av-sync-bridge prototype", nullptr);
        g_object_set(rtpbin_, "sdes", sdes, nullptr);
        gst_structure_free(sdes); // Never publish the library's host-derived default CNAME.
        g_object_set(rtp_sink, "host", args.host.c_str(), "port", static_cast<gint>(args.rtp_port),
                     "sync", FALSE, "async", FALSE, "buffer-size", 16384, nullptr);
        g_object_set(rtcp_sink, "host", args.host.c_str(), "port", static_cast<gint>(args.rtcp_port),
                     "sync", FALSE, "async", FALSE, "buffer-size", 16384, nullptr);
        require(gst_element_link_many(source_, quantize, wire_caps, pay_, nullptr),
                "link_conversion_pipeline");
        GstPtr<GstPad> send_rtp(gst_element_request_pad_simple(rtpbin_, "send_rtp_sink_0"));
        GstPtr<GstPad> pay_src(gst_element_get_static_pad(pay_, "src"));
        require(send_rtp && pay_src && gst_pad_link(pay_src.get(), send_rtp.get()) == GST_PAD_LINK_OK,
                "link_rtp_session_input");
        GstPtr<GstPad> rtp_src(gst_element_get_static_pad(rtpbin_, "send_rtp_src_0"));
        GstPtr<GstPad> rtp_in(gst_element_get_static_pad(rtp_sink, "sink"));
        require(rtp_src && rtp_in && gst_pad_link(rtp_src.get(), rtp_in.get()) == GST_PAD_LINK_OK,
                "link_rtp_output");
        GstPtr<GstPad> rtcp_src(gst_element_request_pad_simple(rtpbin_, "send_rtcp_src_0"));
        GstPtr<GstPad> rtcp_in(gst_element_get_static_pad(rtcp_sink, "sink"));
        require(rtcp_src && rtcp_in && gst_pad_link(rtcp_src.get(), rtcp_in.get()) == GST_PAD_LINK_OK,
                "link_rtcp_output");
        gst_pad_add_probe(rtp_src.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER |
                          GST_PAD_PROBE_TYPE_BUFFER_LIST), rtp_probe, this, nullptr);
        gst_pad_add_probe(rtcp_src.get(), GST_PAD_PROBE_TYPE_BUFFER, rtcp_probe, &stats_, nullptr);
        GObject *rtp_session = nullptr;
        g_signal_emit_by_name(rtpbin_, "get-internal-session", 0, &rtp_session);
        require(rtp_session != nullptr, "get_internal_rtp_session");
        g_object_set(rtp_session, "rtcp-min-interval", static_cast<guint64>(500 * GST_MSECOND),
                     "internal-ssrc", ssrc, nullptr);
        g_object_unref(rtp_session);
        gst_pipeline_use_clock(GST_PIPELINE(pipeline_.get()), clock);
        gst_element_set_start_time(pipeline_.get(), GST_CLOCK_TIME_NONE);
        gst_element_set_base_time(pipeline_.get(), 0);
        bus_.reset(gst_element_get_bus(pipeline_.get()));
        require(gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE,
                "start_sender_pipeline");
    }
    ~SenderPipeline()
    {
        // Drop queued PCM immediately on reset/stop. No drain of old audio into
        // a fresh SSRC, and no implicit presentation delay is accumulated.
        if (pipeline_) gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
    }
    bool overflowed() const { return overflow_.load(); }
    void check_bus()
    {
        require(!wire_fault_.load(), "anchor_transport_failed");
        for (unsigned i = 0; i < 64; ++i) {
            auto *message = gst_bus_pop(bus_.get());
            if (!message) return;
            const bool failed = GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR;
            std::uint32_t code = 0;
            if (failed) {
                GError *error = nullptr;
                gst_message_parse_error(message, &error, nullptr);
                if (error) { code = static_cast<std::uint32_t>(error->code); g_error_free(error); }
            }
            gst_message_unref(message);
            if (failed) throw Failure{"gstreamer_pipeline_error", code};
        }
    }
    bool push(const Capture &capture, const Packet &packet, const avsync::net::MappedCapture& mapping,
              std::int64_t shared_now, GstClock *clock, Steady::time_point deadline)
    {
        const auto mapped_ns = mapping.capture_ns;
        if (!anchors_) {
            avsync::AudioAnchorConfig config;
            config.rate.nominal_rate = capture.format().rate;
            config.device_frame_origin = packet.device_position;
            config.max_anchor_age_ns = max_age_ns;
            config.max_future_ns = capture_window.maximum_future_ns;
            anchors_.emplace(epoch_, config);
            nominal_timeline_.emplace(mapped_ns, 48'000);
            device_origin_ = packet.device_position;
        }
        // Original anchors and nominal RTP sample time are distinct ledgers.
        const auto observation = anchors_->observe({epoch_, anchor_sequence_++, packet.device_position,
            mapped_ns, capture.format().rate, false}, shared_now);
        require(!anchors_->faulted(), "original_capture_anchor_invalid");
        ++stats_.anchor_observations;
        if (observation.estimate) {
            ++stats_.anchor_measurements;
            stats_.last_original_rate_ppm = observation.estimate->source_rate_error_ppm;
            if (!observation.estimate->within_correction_limit) ++stats_.anchor_out_of_range;
        }
        if (!last_selected_position_ || packet.device_position - *last_selected_position_ >=
                (capture.format().rate + 49) / 50) {
            avsync::wire::AudioRecord record{};
            record.epoch = epoch_;
            record.clock_epoch = clock_epoch_;
            record.device_origin = device_origin_;
            record.source_rate = capture.format().rate;
            record.anchor_sequence = selected_sequence_++;
            record.device_position = packet.device_position;
            record.capture_ns = mapped_ns;
            record.qpc_100ns = packet.qpc_100ns;
            record.calibration_revision = mapping.revision;
            std::lock_guard lock(ledger_mutex_);
            require(ledger_size_ < ledger_.size(), "anchor_ledger_capacity");
            ledger_[(ledger_head_ + ledger_size_) % ledger_.size()] = record;
            if (!record.anchor_sequence) wire_origin_time_ = record.capture_ns;
            ++ledger_size_;
            last_selected_position_ = packet.device_position;
            ++stats_.selected_anchors;
            stats_.last_selected_device_position = record.device_position;
            stats_.last_selected_sequence = record.anchor_sequence;
            stats_.last_selected_qpc_100ns = record.qpc_100ns;
            stats_.last_selected_capture_ns = record.capture_ns;
        }
        stats_.calibration_revision = mapping.revision;
        const auto started = Steady::now();
        const auto converted = converter_.process(
            {reinterpret_cast<const std::byte*>(capture.data()), packet.bytes}, packet.frames, converted_);
        stats_.maximum_conversion_ns = std::max(stats_.maximum_conversion_ns,
            static_cast<std::int64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Steady::now()-started).count()));
        require((converted.status == avsync::audio::NominalAudioStatus::progress ||
                 converted.status == avsync::audio::NominalAudioStatus::no_progress) &&
                converted.input_frames_used == packet.frames, "nominal_conversion_failed");
        stats_.converted_input_frames += converted.input_frames_used;
        stats_.converted_output_frames += converted.output_frames_generated;
        if (!converted.output_frames_generated) return true;
        require(converted.output_frames_generated <= converted_.size()/2 &&
                wire_frames_ <= std::numeric_limits<std::uint64_t>::max()-converted.output_frames_generated,
                "nominal_conversion_size");
        const auto end_frame = wire_frames_ + converted.output_frames_generated;
        const auto pts = nominal_timeline_->at(wire_frames_);
        const auto end = nominal_timeline_->at(end_frame);
        require(pts && end, "nominal_timeline_overflow");
        const auto bytes = converted.output_frames_generated * 2 * sizeof(float);
        BufferPtr buffer(gst_buffer_new_allocate(nullptr, bytes, nullptr));
        require(buffer != nullptr && gst_buffer_fill(buffer.get(), 0, converted_.data(), bytes) == bytes,
                "allocate_converted_buffer");
        GST_BUFFER_PTS(buffer.get()) = static_cast<GstClockTime>(*pts);
        GST_BUFFER_DTS(buffer.get()) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DURATION(buffer.get()) = static_cast<GstClockTime>(*end-*pts);
        GST_BUFFER_OFFSET(buffer.get()) = wire_frames_;
        GST_BUFFER_OFFSET_END(buffer.get()) = end_frame;
        wire_frames_ = end_frame;
        if (first_) { GST_BUFFER_FLAG_SET(buffer.get(), GST_BUFFER_FLAG_DISCONT); first_ = false; }
        const auto after_conversion = gst_clock_get_time(clock);
        if (Steady::now() >= deadline ||
            after_conversion > static_cast<guint64>(std::numeric_limits<std::int64_t>::max()) ||
            avsync::capture_window_status(mapped_ns, static_cast<std::int64_t>(after_conversion), capture_window) !=
                avsync::CaptureWindowStatus::accepted) return false;
        const auto flow = gst_app_src_push_buffer(GST_APP_SRC(source_), buffer.release());
        require(flow == GST_FLOW_OK, "appsrc_push_failed");
        return true;
    }
private:
    GstElement *make(const char *factory, const char *name)
    {
        auto *element = gst_element_factory_make(factory, name);
        require(element != nullptr, "missing_gstreamer_element");
        if (!gst_bin_add(GST_BIN(pipeline_.get()), element)) {
            gst_object_unref(element);
            throw Failure{"add_pipeline_element"};
        }
        return element;
    }
    static void enough_data(GstElement *, gpointer opaque)
    {
        static_cast<SenderPipeline *>(opaque)->overflow_.store(true);
    }
    bool decorate_rtp(GstBuffer*& buffer)
    {
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!buffer || !gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) return false;
        const auto payload = gst_rtp_buffer_get_payload_len(&rtp);
        const auto stamp = gst_rtp_buffer_get_timestamp(&rtp);
        gst_rtp_buffer_unmap(&rtp);
        if (!payload || payload % 6 || payload/6 > 180) return false;
        if (!extension_frames_) extension_rtp_zero_ = stamp;
        if (stamp != static_cast<guint32>(extension_rtp_zero_ + extension_frames_)) return false;
        avsync::wire::AudioRecord record{};
        {
            std::lock_guard lock(ledger_mutex_);
            if (!ledger_size_) return false;
            // Select by exact nominal association, never by latest arrival.
            while (ledger_size_ > 1) {
                const auto& next = ledger_[(ledger_head_ + 1) % ledger_.size()];
                const auto position = avsync::nominal_wire_position(next.device_position,
                    next.device_origin, 0, next.source_rate);
                if (!position) return false;
                if (position->whole > extension_frames_ ||
                    (position->whole == extension_frames_ && position->remainder)) break;
                ledger_head_ = (ledger_head_ + 1) % ledger_.size();
                --ledger_size_;
            }
            record = ledger_[ledger_head_];
            const auto pts = avsync::RationalTimeline(wire_origin_time_, 48'000).at(extension_frames_);
            const auto actual = GST_BUFFER_PTS(buffer);
            if (!pts || !GST_CLOCK_TIME_IS_VALID(actual) ||
                actual > static_cast<guint64>(std::numeric_limits<std::int64_t>::max()) ||
                std::abs(static_cast<std::int64_t>(actual) - *pts) > 1) return false;
        }
        if ((!advertised_anchor_ && record.anchor_sequence != 0) ||
            (advertised_anchor_ && record.anchor_sequence != *advertised_anchor_ &&
             record.anchor_sequence != *advertised_anchor_ + 1)) return false;
        const auto now = gst_clock_get_time(clock_);
        if (now > static_cast<guint64>(std::numeric_limits<std::int64_t>::max()) ||
            avsync::capture_window_status(record.capture_ns, static_cast<std::int64_t>(now),
                {100'000'000, 200'000'000}) != avsync::CaptureWindowStatus::accepted) return false;
        record.packet_wire_start = extension_frames_;
        record.rtp_zero = extension_rtp_zero_;
        if (!avsync::net::add_audio_anchor(buffer, record)) return false;
        advertised_anchor_ = record.anchor_sequence;
        extension_frames_ += payload/6;
        return true;
    }
    void count_rtp(GstBuffer *buffer)
    {
        auto &stats = stats_;
        GstRTPBuffer rtp = GST_RTP_BUFFER_INIT;
        if (!gst_rtp_buffer_map(buffer, GST_MAP_READ, &rtp)) { ++stats.rtp_invalid_payload; return; }
        const auto count = stats.rtp_packets.fetch_add(1);
        stats.rtp_payload_bytes.fetch_add(gst_rtp_buffer_get_payload_len(&rtp));
        stats.last_rtp_timestamp.store(gst_rtp_buffer_get_timestamp(&rtp));
        const auto pts = GST_BUFFER_PTS(buffer);
        const auto payload = gst_rtp_buffer_get_payload_len(&rtp);
        if (!payload || payload % 6) ++stats.rtp_invalid_payload;
        const auto stamp = gst_rtp_buffer_get_timestamp(&rtp);
        if (!rtp_timeline_) {
            if (GST_CLOCK_TIME_IS_VALID(pts) && pts <= static_cast<guint64>(std::numeric_limits<std::int64_t>::max()))
                rtp_timeline_.emplace(static_cast<std::int64_t>(pts), 48'000);
            rtp_origin_ = stamp;
        }
        if (stamp != static_cast<guint32>(rtp_origin_ + rtp_frames_)) ++stats.rtp_frame_steps;
        const auto expected = rtp_timeline_ ? rtp_timeline_->at(rtp_frames_) : std::nullopt;
        if (!expected || !GST_CLOCK_TIME_IS_VALID(pts) ||
            pts > static_cast<guint64>(std::numeric_limits<std::int64_t>::max()) ||
            std::abs(static_cast<std::int64_t>(pts)-*expected) > 1) ++stats.rtp_nominal_pts_steps;
        rtp_frames_ += payload/6;
        if (GST_CLOCK_TIME_IS_VALID(pts)) {
            if (!count) stats.first_rtp_pts.store(pts);
            stats.last_rtp_pts.store(pts);
        }
        gst_rtp_buffer_unmap(&rtp);
    }
    static GstPadProbeReturn rtp_probe(GstPad *, GstPadProbeInfo *info, gpointer opaque)
    {
        auto &self = *static_cast<SenderPipeline *>(opaque);
        if (self.wire_fault_.load()) return GST_PAD_PROBE_DROP;
        try {
            bool valid = true;
            if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER) {
                auto* buffer = GST_PAD_PROBE_INFO_BUFFER(info);
                valid = self.decorate_rtp(buffer);
                GST_PAD_PROBE_INFO_DATA(info) = buffer;
                if (valid) { self.count_rtp(buffer); ++self.stats_.anchored_rtp_packets; }
            } else if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER_LIST) {
                auto* list = gst_buffer_list_make_writable(GST_PAD_PROBE_INFO_BUFFER_LIST(info));
                GST_PAD_PROBE_INFO_DATA(info) = list;
                valid = list && gst_buffer_list_length(list) <= 256;
                for (guint i = 0; valid && i < gst_buffer_list_length(list); ++i) {
                    auto* buffer = gst_buffer_ref(gst_buffer_list_get(list, i));
                    try { valid = self.decorate_rtp(buffer); }
                    catch (...) { if (buffer) gst_buffer_unref(buffer); throw; }
                    if (!valid) { if (buffer) gst_buffer_unref(buffer); break; }
                    gst_buffer_list_remove(list, i, 1);
                    gst_buffer_list_insert(list, static_cast<gint>(i), buffer);
                }
                if (valid) for (guint i = 0; i < gst_buffer_list_length(list); ++i) {
                    self.count_rtp(gst_buffer_list_get(list, i));
                    ++self.stats_.anchored_rtp_packets;
                }
            }
            if (valid) return GST_PAD_PROBE_OK;
        } catch (...) {
            // Never unwind across a C streaming callback.
        }
        ++self.stats_.anchor_transport_errors;
        self.wire_fault_.store(true);
        return GST_PAD_PROBE_DROP;
    }
    static GstPadProbeReturn rtcp_probe(GstPad *, GstPadProbeInfo *info, gpointer opaque)
    {
        auto &stats = *static_cast<Statistics *>(opaque);
        GstRTCPBuffer rtcp = GST_RTCP_BUFFER_INIT;
        if (!gst_rtcp_buffer_map(GST_PAD_PROBE_INFO_BUFFER(info), GST_MAP_READ, &rtcp)) return GST_PAD_PROBE_OK;
        stats.rtcp_packets.fetch_add(1);
        GstRTCPPacket packet;
        if (gst_rtcp_buffer_get_first_packet(&rtcp, &packet)) {
            do {
                if (gst_rtcp_packet_get_type(&packet) == GST_RTCP_TYPE_SR) {
                    guint32 ssrc, rtptime, count, octets;
                    guint64 ntptime;
                    gst_rtcp_packet_sr_get_sender_info(&packet, &ssrc, &ntptime, &rtptime, &count, &octets);
                    stats.sender_reports.fetch_add(1);
                    stats.last_sr_ntp64.store(ntptime);
                    stats.last_sr_rtp_timestamp.store(rtptime);
                }
            } while (gst_rtcp_packet_move_to_next(&packet));
        }
        gst_rtcp_buffer_unmap(&rtcp);
        return GST_PAD_PROBE_OK;
    }
    Statistics &stats_;
    avsync::audio::NominalAudioConverter converter_;
    std::vector<float> converted_;
    avsync::SessionToken epoch_;
    std::uint64_t clock_epoch_{};
    GstClock* clock_{}; // Borrowed; owning client outlives this pipeline.
    std::uint64_t device_origin_{}, selected_sequence_{}, extension_frames_{};
    std::optional<std::uint64_t> last_selected_position_, advertised_anchor_;
    guint32 extension_rtp_zero_{};
    std::mutex ledger_mutex_;
    std::array<avsync::wire::AudioRecord, 32> ledger_{};
    std::size_t ledger_head_{}, ledger_size_{};
    std::int64_t wire_origin_time_{};
    std::atomic<bool> wire_fault_{false};
    std::optional<avsync::AudioAnchorTracker> anchors_;
    std::optional<avsync::RationalTimeline> nominal_timeline_, rtp_timeline_;
    std::uint64_t anchor_sequence_{}, wire_frames_{}, rtp_frames_{};
    guint32 rtp_origin_{};
    GstPtr<GstBus> bus_;
    GstElement *source_{}, *pay_{}, *rtpbin_{};
    std::atomic<bool> overflow_{false};
    bool first_ = true;
    // Destroy first during constructor unwinding too: streaming callbacks must
    // stop while their owning state/counters are still alive.
    std::unique_ptr<GstElement, PipelineDeleter> pipeline_;
};

void clock_messages(GstBus *bus, avsync::net::ClockHealthMonitor &health)
{
    for (unsigned i = 0; i < 64; ++i) {
        auto *message = gst_bus_pop(bus);
        if (!message) return;
        (void)health.observe(message);
        gst_message_unref(message);
    }
}

void write_summary(const Statistics &s, const CaptureFormat *format, const avsync::net::ClockHealth &health,
                   const char *status, const char *error_stage = nullptr, std::uint32_t error_code = 0)
{
    std::cout << "{\"schema\":1,\"status\":\"" << status << "\",\"desktop_only\":true,\"receiver_verified\":false"
              << ",\"timestamp_semantics\":\"nominal_media_plus_original_anchor_extension\",\"original_anchors_transmitted\":"
              << (s.anchored_rtp_packets.load() ? "true" : "false")
              << ",\"adaptive_correction\":false"
              << ",\"clock_usable\":" << (health.usable ? "true" : "false")
              << ",\"clock_reason\":\"" << health.reason << '"'
              << ",\"clock_observations\":" << health.observations << ",\"domain_bracket_ns\":" << s.domain_bracket_ns;
    if (health.observation_age_ns) std::cout << ",\"clock_observation_age_ns\":" << *health.observation_age_ns;
    if (health.rtt_ns) std::cout << ",\"clock_rtt_ns\":" << *health.rtt_ns;
    if (format) std::cout << ",\"input_channels\":" << format->channels << ",\"input_rate\":" << format->rate
                          << ",\"windows_channel_mask\":" << format->windows_mask;
    std::cout << ",\"captured_packets\":" << s.captured_packets << ",\"captured_frames\":" << s.captured_frames
              << ",\"silent_packets\":" << s.silent_packets << ",\"initial_discontinuities\":" << s.initial_discontinuities
              << ",\"discontinuities\":" << s.discontinuities << ",\"timestamp_errors\":" << s.timestamp_errors
              << ",\"dropped_packets\":" << s.dropped_packets << ",\"resets\":" << s.resets
              << ",\"queue_overflows\":" << s.queue_overflows << ",\"mapped_packets\":" << s.mapped_packets
              << ",\"converted_input_frames\":" << s.converted_input_frames
              << ",\"converted_output_frames\":" << s.converted_output_frames
              << ",\"converter_latency_input_frames\":" << s.converter_latency_input_frames
              << ",\"maximum_conversion_ns\":" << s.maximum_conversion_ns
              << ",\"original_anchor_observations\":" << s.anchor_observations
              << ",\"original_anchor_measurements\":" << s.anchor_measurements
              << ",\"original_anchor_out_of_range\":" << s.anchor_out_of_range
              << ",\"last_original_rate_ppm\":" << s.last_original_rate_ppm
              << ",\"rtp_frame_steps\":" << s.rtp_frame_steps.load()
              << ",\"rtp_nominal_pts_steps\":" << s.rtp_nominal_pts_steps.load()
              << ",\"rtp_invalid_payload\":" << s.rtp_invalid_payload.load()
              << ",\"selected_original_anchors\":" << s.selected_anchors
              << ",\"calibration_revision\":" << s.calibration_revision
              << ",\"anchored_rtp_packets\":" << s.anchored_rtp_packets.load()
              << ",\"anchor_transport_errors\":" << s.anchor_transport_errors.load()
              << ",\"last_selected_device_position\":" << s.last_selected_device_position
              << ",\"last_selected_anchor_sequence\":" << s.last_selected_sequence
              << ",\"last_selected_qpc_100ns\":" << s.last_selected_qpc_100ns
              << ",\"last_selected_capture_ns\":" << s.last_selected_capture_ns
              << ",\"reject_mapping\":" << s.reject_mapping << ",\"reject_clock_now\":" << s.reject_clock_now
              << ",\"reject_future\":" << s.reject_future << ",\"reject_stale\":" << s.reject_stale
              << ",\"reject_nonmonotonic\":" << s.reject_nonmonotonic
              << ",\"reject_unhealthy\":" << s.reject_unhealthy << ",\"reject_after_build\":" << s.reject_after_build
              << ",\"reject_after_conversion\":" << s.reject_after_conversion
              << ",\"first_device_position\":" << s.first_device_position << ",\"last_device_position\":" << s.last_device_position
              << ",\"first_qpc_100ns\":" << s.first_qpc_100ns << ",\"last_qpc_100ns\":" << s.last_qpc_100ns
              << ",\"first_capture_ns\":" << s.first_capture_ns << ",\"last_capture_ns\":" << s.last_capture_ns
              << ",\"ssrc\":" << s.last_ssrc << ",\"rtp_packets_at_output\":" << s.rtp_packets.load()
              << ",\"rtp_payload_bytes\":" << s.rtp_payload_bytes.load() << ",\"rtcp_packets_at_output\":" << s.rtcp_packets.load()
              << ",\"sender_reports\":" << s.sender_reports.load() << ",\"first_rtp_pts_ns\":" << s.first_rtp_pts.load()
              << ",\"last_rtp_pts_ns\":" << s.last_rtp_pts.load() << ",\"last_rtp_timestamp\":" << s.last_rtp_timestamp.load()
              << ",\"last_sr_clock_32_32\":" << s.last_sr_ntp64.load()
              << ",\"last_sr_rtp_timestamp\":" << s.last_sr_rtp_timestamp.load();
    if (s.minimum_mapped_minus_now) std::cout << ",\"minimum_mapped_minus_now_ns\":" << *s.minimum_mapped_minus_now;
    if (s.maximum_mapped_minus_now) std::cout << ",\"maximum_mapped_minus_now_ns\":" << *s.maximum_mapped_minus_now;
    if (s.minimum_qpc_minus_internal) std::cout << ",\"minimum_qpc_minus_internal_ns\":" << *s.minimum_qpc_minus_internal;
    if (s.maximum_qpc_minus_internal) std::cout << ",\"maximum_qpc_minus_internal_ns\":" << *s.maximum_qpc_minus_internal;
    std::cout<<",\"clock_poll_timeout_ns\":"<<s.clock_poll_timeout_ns
        <<",\"clock_loss_count\":"<<s.clock_losses<<",\"first_clock_losses\":[";
    for (std::size_t i=0;i<std::min<std::uint64_t>(s.clock_losses,s.first_clock_losses.size());++i) {
        const auto& loss=s.first_clock_losses[i]; const auto& h=loss.health;
        if (i) std::cout<<',';
        std::cout<<"{\"reason\":\""<<h.reason<<"\",\"elapsed_ms\":"<<loss.elapsed_ms
            <<",\"mapped_packets\":"<<loss.mapped_packets<<",\"observations\":"<<h.observations;
        if (h.observation_age_ns) std::cout<<",\"observation_age_ns\":"<<*h.observation_age_ns;
        if (h.rtt_ns) std::cout<<",\"rtt_ns\":"<<*h.rtt_ns;
        if (h.rate_error_ppm) std::cout<<",\"rate_ppm\":"<<*h.rate_error_ppm;
        std::cout<<'}';
    }
    std::cout<<']';
    const auto write_rejection = [](const Rejection &r) {
        std::cout << "{\"reason\":\"" << r.reason << "\",\"device_position\":" << r.device_position
                  << ",\"qpc_100ns\":" << r.qpc_100ns << ",\"network_now_ns\":" << r.network_now
                  << ",\"raw_internal_now_ns\":" << r.raw_internal_now;
        if (r.qpc_ns) std::cout << ",\"qpc_ns\":" << *r.qpc_ns;
        if (r.mapped_ns) std::cout << ",\"mapped_ns\":" << *r.mapped_ns;
        if (r.calibration_snapshot) {
            const auto &c = *r.calibration_snapshot;
            std::cout << ",\"calibration_snapshot\":{\"internal_ns\":" << c.internal_reference
                      << ",\"external_ns\":" << c.external_reference << ",\"numerator\":" << c.rate_numerator
                      << ",\"denominator\":" << c.rate_denominator << '}';
        }
        std::cout << '}';
    };
    std::cout << ",\"first_rejections\":[";
    for (std::size_t i = 0; i < s.rejection_count; ++i) {
        if (i) std::cout << ',';
        write_rejection(s.first_rejections[i]);
    }
    std::cout << "],\"last_rejection\":";
    if (s.last_rejection) write_rejection(*s.last_rejection);
    else std::cout << "null";
    if (error_stage) std::cout << ",\"error_stage\":\"" << error_stage << "\",\"error_code\":" << error_code;
    std::cout << "}\n";
}

int run(const Arguments &args)
{
    Statistics stats;
    std::optional<CaptureFormat> observed_format;
    avsync::net::ClockHealthMonitor monitor;
    avsync::net::CaptureClockMapper mapper;
    avsync::net::ClockHealth health{};
    try {
        const auto started = Steady::now();
        const auto deadline = started + std::chrono::seconds(args.seconds);
        GError *error = nullptr;
        if (!gst_init_check(nullptr, nullptr, &error)) {
            const auto code = error ? static_cast<std::uint32_t>(error->code) : 0;
            if (error) g_error_free(error);
            throw Failure{"gstreamer_initialize", code};
        }
        ComScope com;
        std::uint64_t session{};
        do { session = (static_cast<std::uint64_t>(g_random_int()) << 32) | g_random_int(); } while (!session);
        GstPtr<GstBus> clock_bus(gst_bus_new());
        GstPtr<GstClock> clock(gst_net_client_clock_new("avsync-shared-clock", args.host.c_str(),
                                                       static_cast<gint>(args.clock_port), 0));
        require(clock && clock_bus, "create_network_clock");
        g_object_set(clock.get(), "bus", clock_bus.get(), "minimum-update-interval", static_cast<guint64>(100 * GST_MSECOND),
                     "round-trip-limit", static_cast<guint64>(5 * GST_MSECOND), nullptr);
        GstClock *raw_internal = nullptr;
        g_object_get(clock.get(), "internal-clock", &raw_internal, nullptr);
        GstPtr<GstClock> internal(raw_internal);
        require(internal != nullptr, "missing_internal_network_clock");
        // The NetClientClock wrapper's inherited timeout is NOT the polling
        // clock. Bound the actual internal clock's adaptive polling/retry wait
        // to 250 ms, leaving headroom inside the unchanged 2-second health gate.
        // Upstream may still reject RTT outliers; this is not an error bound.
        gst_clock_set_timeout(internal.get(),250*GST_MSECOND);
        stats.clock_poll_timeout_ns=gst_clock_get_timeout(internal.get());
        require(stats.clock_poll_timeout_ns==250*GST_MSECOND,"clock_poll_timeout");
        const auto domain = avsync::net::verify_local_monotonic_domain(internal.get());
        stats.domain_bracket_ns = domain.bracket_width_ns;
        require(domain.valid, "qpc_gstreamer_domain_mismatch");
        while (Steady::now() < deadline) {
            clock_messages(clock_bus.get(), monitor);
            health = monitor.health(clock.get());
            if (health.usable) break;
            gst_clock_wait_for_sync(clock.get(), 50 * GST_MSECOND);
            Sleep(10);
        }
        if (!health.usable || Steady::now() >= deadline) {
            write_summary(stats, nullptr, health, "waiting_clock");
            return 3;
        }
        Capture capture;
        observed_format = capture.format();
        std::unique_ptr<SenderPipeline> pipeline;
        std::optional<std::uint64_t> expected_position;
        std::optional<std::int64_t> last_mapped;
        bool clock_was_usable = true;
        auto reset_pipeline = [&] {
            if (pipeline) {
                pipeline.reset();
                ++stats.resets;
                require(stats.resets <= max_resets, "restart_limit");
            }
            last_mapped.reset();
        };
        pipeline = std::make_unique<SenderPipeline>(args, capture.format(), clock.get(), stats, session);
        if (Steady::now() >= deadline) throw Failure{"deadline_before_capture"};
        capture.start();
        while (Steady::now() < deadline) {
            clock_messages(clock_bus.get(), monitor);
            health = monitor.health(clock.get());
            if (clock_was_usable && !health.usable) {
                if (stats.clock_losses<stats.first_clock_losses.size())
                    stats.first_clock_losses[stats.clock_losses]={health,static_cast<std::uint64_t>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(Steady::now()-started).count()),stats.mapped_packets};
                ++stats.clock_losses;
                // Require a fresh observation window after health loss. Reset
                // once on the edge, not repeatedly while still acquiring.
                monitor.reset();
                health = monitor.health(clock.get());
            }
            clock_was_usable = health.usable;
            if (pipeline) {
                pipeline->check_bus();
                if (pipeline->overflowed()) { ++stats.queue_overflows; reset_pipeline(); }
            }
            if (!health.usable) reset_pipeline();
            Packet packet;
            if (!capture.next(packet)) { Sleep(2); continue; }
            ++stats.captured_packets;
            stats.captured_frames += packet.frames;
            if (packet.flags & AUDCLNT_BUFFERFLAGS_SILENT) ++stats.silent_packets;
            bool discontinuity = false;
            if (packet.flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                if (stats.captured_packets == 1) ++stats.initial_discontinuities;
                else discontinuity = true;
            }
            if (packet.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) {
                ++stats.timestamp_errors;
                ++stats.dropped_packets;
                expected_position.reset();
                reset_pipeline();
                continue;
            }
            require(packet.device_position <= std::numeric_limits<std::uint64_t>::max() - packet.frames,
                    "device_position_overflow");
            if (expected_position && *expected_position != packet.device_position) discontinuity = true;
            expected_position = packet.device_position + packet.frames;
            if (discontinuity) { ++stats.discontinuities; ++stats.dropped_packets; reset_pipeline(); continue; }
            if (!health.usable) { ++stats.dropped_packets; ++stats.reject_unhealthy; reset_pipeline(); continue; }
            const auto mapping = mapper.map(clock.get(), packet.qpc_100ns);
            const auto mapped = mapping ? std::optional{mapping->capture_ns} : std::nullopt;
            const auto now = gst_clock_get_time(clock.get());
            const bool valid_now = now <= static_cast<guint64>(std::numeric_limits<std::int64_t>::max());
            if (mapped && *mapped >= 0 && valid_now)
                range_observation(stats.minimum_mapped_minus_now, stats.maximum_mapped_minus_now,
                                  *mapped - static_cast<std::int64_t>(now));
            const char *reject = nullptr;
            if (!mapped || *mapped < 0) { reject = "mapping_invalid"; ++stats.reject_mapping; }
            else if (!valid_now) { reject = "clock_now_invalid"; ++stats.reject_clock_now; }
            else if (avsync::capture_window_status(*mapped, static_cast<std::int64_t>(now), capture_window) ==
                     avsync::CaptureWindowStatus::too_far_future) { reject = "future_over_bound"; ++stats.reject_future; }
            else if (avsync::capture_window_status(*mapped, static_cast<std::int64_t>(now), capture_window) ==
                     avsync::CaptureWindowStatus::too_old) { reject = "stale"; ++stats.reject_stale; }
            else if (last_mapped && *mapped <= *last_mapped) { reject = "nonmonotonic"; ++stats.reject_nonmonotonic; }
            if (reject) {
                record_rejection(stats, reject, packet, mapped, now, clock.get(), internal.get());
                ++stats.dropped_packets;
                reset_pipeline();
                continue;
            }
            if (!pipeline) pipeline = std::make_unique<SenderPipeline>(args, capture.format(), clock.get(), stats, session);
            // A rebuild can consume time. Recheck the deadline and capture age,
            // never make an old packet current by changing its timestamp.
            const auto after_build = gst_clock_get_time(clock.get());
            if (Steady::now() >= deadline || after_build > static_cast<guint64>(std::numeric_limits<std::int64_t>::max()) ||
                avsync::capture_window_status(*mapped, static_cast<std::int64_t>(after_build), capture_window) !=
                    avsync::CaptureWindowStatus::accepted) {
                ++stats.dropped_packets;
                ++stats.reject_after_build;
                record_rejection(stats, "after_build_deadline_or_age", packet, mapped, after_build, clock.get(), internal.get());
                reset_pipeline();
                continue;
            }
            if (!pipeline->push(capture, packet, *mapping, static_cast<std::int64_t>(after_build), clock.get(), deadline)) {
                ++stats.dropped_packets;
                ++stats.reject_after_conversion;
                record_rejection(stats, "after_conversion_deadline_or_age", packet, mapped,
                    gst_clock_get_time(clock.get()), clock.get(), internal.get());
                reset_pipeline();
                continue;
            }
            last_mapped = mapped;
            if (!stats.mapped_packets) {
                stats.first_device_position = packet.device_position;
                stats.first_qpc_100ns = packet.qpc_100ns;
                stats.first_capture_ns = *mapped;
            }
            ++stats.mapped_packets;
            stats.last_device_position = packet.device_position;
            stats.last_qpc_100ns = packet.qpc_100ns;
            stats.last_capture_ns = *mapped;
        }
        capture.stop();
        pipeline.reset();
        clock_messages(clock_bus.get(), monitor);
        health = monitor.health(clock.get());
        const bool invalid_timing = stats.rtp_frame_steps.load() || stats.rtp_nominal_pts_steps.load() ||
                                    stats.rtp_invalid_payload.load() || stats.anchor_transport_errors.load();
        write_summary(stats, &*observed_format, health, invalid_timing ? "invalid_nominal_timing" :
                      stats.rtp_packets.load() ? "rtp_output_observed_unverified" : "waiting_no_rtp");
        return stats.rtp_packets.load() && !invalid_timing ? 0 : 3;
    } catch (const Failure &failure) {
        write_summary(stats, observed_format ? &*observed_format : nullptr, health, "error", failure.stage, failure.code);
        return 1;
    } catch (...) {
        write_summary(stats, observed_format ? &*observed_format : nullptr, health, "error", "unexpected_exception");
        return 1;
    }
}

void help()
{
    std::cout << "avsync-windows-sender --loopback --host IPV4 --clock-port N --rtp-port N --rtcp-port N --seconds N --clock-epoch N\n"
                 "Experimental desktop-only WASAPI -> explicit stereo mix -> 48 kHz L24 RTP.\n"
                 "All options required; seconds is an overall deadline from 1 to 120.\n"
                 "A numeric unicast IPv4 destination and three distinct ports are required.\n"
                 "Help/no arguments opens no audio endpoint and sends no network traffic.\n"
                 "The explicit run captures desktop PCM and sends it unencrypted to that host.\n"
                 "No microphone, recording files, startup changes, endpoint changes or OBS changes.\n"
                 "Shared-monotonic RTCP convention, not UTC. No sender presentation delay.\n"
                 "Copy the fresh clock-epoch from the receiver READY line for this finite test.\n"
                 "Nominal RTP time is NOT capture time; a versioned extension carries original anchors.\n"
                 "Fixed-rate resampling is not long-run device-clock correction.\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) { help(); return 0; }
    const auto args = parse_arguments(argc, argv);
    if (!args) {
        std::cout << "{\"schema\":1,\"status\":\"error\",\"error_stage\":\"arguments\"}\n";
        return 2;
    }
    return run(*args);
}
