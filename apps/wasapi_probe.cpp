// SPDX-License-Identifier: GPL-2.0-or-later
// Explicit, bounded metadata observation only: no PCM copies, files or network.
#ifndef _WIN32
#error "The WASAPI metadata probe is Windows-only"
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

#include <array>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>

namespace {
using Microsoft::WRL::ComPtr;
using Clock = std::chrono::steady_clock;
constexpr std::size_t max_pairs = 256;
constexpr auto pair_period = std::chrono::milliseconds(250);

struct ProbeError {
    const char *stage;
    HRESULT code;
};

void check(HRESULT code, const char *stage)
{
    if (FAILED(code))
        throw ProbeError{stage, code};
}

class ComScope {
public:
    ComScope() { check(CoInitializeEx(nullptr, COINIT_MULTITHREADED), "CoInitializeEx"); }
    ~ComScope() { CoUninitialize(); }
    ComScope(const ComScope &) = delete;
    ComScope &operator=(const ComScope &) = delete;
};

struct FormatDeleter {
    void operator()(WAVEFORMATEX *value) const { CoTaskMemFree(value); }
};

class StartedStream {
public:
    explicit StartedStream(IAudioClient *client) : client_(client)
    {
        check(client_->Start(), "IAudioClient::Start");
    }
    ~StartedStream() { if (client_) client_->Stop(); }
    void stop()
    {
        auto *client = client_;
        client_ = nullptr;
        check(client->Stop(), "IAudioClient::Stop");
    }
    StartedStream(const StartedStream &) = delete;
    StartedStream &operator=(const StartedStream &) = delete;
private:
    IAudioClient *client_;
};

struct Format {
    unsigned tag{}, channels{}, sample_rate{}, container_bits{}, valid_bits{}, block_align{};
    std::uint32_t bytes_per_second{}, channel_mask{};
    bool extensible{}, channel_mask_known{};
    const char *encoding = "other";
};

Format describe_format(const WAVEFORMATEX &wave)
{
    if (!wave.nChannels || !wave.nSamplesPerSec || !wave.nBlockAlign)
        throw ProbeError{"invalid_mix_format", E_INVALIDARG};
    Format result;
    result.tag = wave.wFormatTag;
    result.channels = wave.nChannels;
    result.sample_rate = wave.nSamplesPerSec;
    result.container_bits = wave.wBitsPerSample;
    result.valid_bits = wave.wBitsPerSample;
    result.block_align = wave.nBlockAlign;
    result.bytes_per_second = wave.nAvgBytesPerSec;
    if (wave.wFormatTag == WAVE_FORMAT_PCM)
        result.encoding = "integer_pcm";
    else if (wave.wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        result.encoding = "ieee_float";
    else if (wave.wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
        if (wave.cbSize < sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
            throw ProbeError{"invalid_extensible_mix_format", E_INVALIDARG};
        const auto &extended = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(wave);
        // Standard WAVEFORMATEXTENSIBLE subtype GUIDs; these are format codes,
        // not endpoint identifiers. No endpoint identity is requested or logged.
        constexpr GUID pcm{WAVE_FORMAT_PCM, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        constexpr GUID ieee_float{WAVE_FORMAT_IEEE_FLOAT, 0x0000, 0x0010,
                                  {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
        result.extensible = true;
        result.valid_bits = extended.Samples.wValidBitsPerSample;
        result.channel_mask = extended.dwChannelMask;
        result.channel_mask_known = extended.dwChannelMask != 0;
        if (IsEqualGUID(extended.SubFormat, pcm))
            result.encoding = "integer_pcm";
        else if (IsEqualGUID(extended.SubFormat, ieee_float))
            result.encoding = "ieee_float";
    }
    return result;
}

struct Pair {
    std::uint64_t packet_index{}, device_position_frames{}, qpc_position_100ns{};
    std::uint32_t frames{}, flags{};
};

struct Observation {
    Format format;
    unsigned requested_seconds{};
    std::uint32_t endpoint_buffer_frames{};
    std::int64_t elapsed_us{}, default_period_100ns{}, minimum_period_100ns{};
    std::uint64_t packets{}, frames{}, silent_packets{}, silent_frames{};
    std::uint64_t discontinuity_packets{}, initial_discontinuity_packets{}, timestamp_error_packets{};
    std::uint64_t empty_polls{}, drain_limit_hits{}, pairs_omitted{};
    std::array<Pair, max_pairs> pairs{};
    std::size_t pair_count{};
    Pair last_packet{};
};

Observation observe_loopback(unsigned seconds)
{
    ComScope com;
    ComPtr<IMMDeviceEnumerator> enumerator;
    check(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                           IID_PPV_ARGS(enumerator.GetAddressOf())), "CoCreateInstance");
    ComPtr<IMMDevice> endpoint;
    check(enumerator->GetDefaultAudioEndpoint(eRender, eConsole, endpoint.GetAddressOf()),
          "GetDefaultAudioEndpoint");
    ComPtr<IAudioClient> audio;
    check(endpoint->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void **>(audio.GetAddressOf())), "IMMDevice::Activate");
    WAVEFORMATEX *raw_format = nullptr;
    check(audio->GetMixFormat(&raw_format), "IAudioClient::GetMixFormat");
    std::unique_ptr<WAVEFORMATEX, FormatDeleter> format(raw_format);
    if (!format)
        throw ProbeError{"missing_mix_format", E_POINTER};
    Observation result;
    result.format = describe_format(*format);
    result.requested_seconds = seconds;
    REFERENCE_TIME default_period = 0, minimum_period = 0;
    check(audio->GetDevicePeriod(&default_period, &minimum_period), "IAudioClient::GetDevicePeriod");
    result.default_period_100ns = default_period;
    result.minimum_period_100ns = minimum_period;
    // Shared-mode loopback, preserving the endpoint's mix format and channel map.
    // The requested 100 ms buffer is a capture allowance, not a render delay or
    // a modification of the endpoint's format, volume, mute or speaker layout.
    check(audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK,
                            1'000'000, 0, format.get(), nullptr), "IAudioClient::Initialize");
    check(audio->GetBufferSize(&result.endpoint_buffer_frames), "IAudioClient::GetBufferSize");
    ComPtr<IAudioCaptureClient> capture;
    check(audio->GetService(IID_PPV_ARGS(capture.GetAddressOf())), "IAudioClient::GetService");
    StartedStream running(audio.Get());
    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::seconds(seconds);
    auto next_pair = begin;
    while (Clock::now() < deadline) {
        unsigned drained = 0;
        for (; drained < 64 && Clock::now() < deadline; ++drained) {
            BYTE *data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            UINT64 device_position = 0, qpc_position = 0;
            const auto hr = capture->GetBuffer(&data, &frames, &flags, &device_position, &qpc_position);
            if (hr == AUDCLNT_S_BUFFER_EMPTY) {
                ++result.empty_polls;
                break; // No position outputs are valid for an empty packet.
            }
            check(hr, "IAudioCaptureClient::GetBuffer");
            if (!frames)
                throw ProbeError{"unexpected_zero_frame_packet", E_UNEXPECTED};
            // Deliberately do not inspect, copy, encode, retain or send 'data'.
            // Consume the complete packet before doing any output or allocation.
            check(capture->ReleaseBuffer(frames), "IAudioCaptureClient::ReleaseBuffer");
            ++result.packets;
            result.frames += frames;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) {
                ++result.silent_packets;
                result.silent_frames += frames;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                ++result.discontinuity_packets;
                if (result.packets == 1)
                    ++result.initial_discontinuity_packets;
            }
            if (flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR)
                ++result.timestamp_error_packets;
            result.last_packet = {result.packets, device_position, qpc_position, frames, flags};
            const auto now = Clock::now();
            const bool retain = result.packets <= 8 || now >= next_pair ||
                (flags & (AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY | AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR));
            if (retain) {
                if (result.pair_count < result.pairs.size())
                    result.pairs[result.pair_count++] = result.last_packet;
                else
                    ++result.pairs_omitted;
                next_pair = now + pair_period;
            }
        }
        if (drained == 64)
            ++result.drain_limit_hits;
        Sleep(2); // Timer-driven metadata probe; no global timer or priority changes.
    }
    result.elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - begin).count();
    running.stop();
    return result;
}

std::string hex32(std::uint32_t value)
{
    std::ostringstream text;
    text << "0x" << std::hex << std::setw(8) << std::setfill('0') << value;
    return text.str();
}

void write_pair(const Pair &pair)
{
    std::cout << "{\"packet_index\":" << pair.packet_index << ",\"frames\":" << pair.frames
              << ",\"flags\":" << pair.flags << ",\"device_position_frames\":" << pair.device_position_frames
              << ",\"qpc_position_100ns\":" << pair.qpc_position_100ns
              << ",\"timestamp_error\":" << ((pair.flags & AUDCLNT_BUFFERFLAGS_TIMESTAMP_ERROR) ? "true" : "false")
              << '}';
}

void write_result(const Observation &r)
{
    const char *status = !r.packets ? "waiting_no_packets" :
        r.silent_packets == r.packets ? "silence_flagged_packets_only" : "packets_observed";
    const auto &f = r.format;
    std::cout << "{\n  \"schema\":1,\"mode\":\"default_render_loopback_metadata\",\"endpoint_role\":\"console\",\n"
              << "  \"status\":\"" << status << "\",\"pcm_inspected\":false,\"pcm_saved\":false,\"network_used\":false,\n"
              << "  \"requested_seconds\":" << r.requested_seconds << ",\"elapsed_us\":" << r.elapsed_us << ",\n"
              << "  \"format\":{\"encoding\":\"" << f.encoding << "\",\"format_tag\":" << f.tag
              << ",\"extensible\":" << (f.extensible ? "true" : "false")
              << ",\"channels\":" << f.channels << ",\"sample_rate\":" << f.sample_rate
              << ",\"container_bits\":" << f.container_bits << ",\"valid_bits\":" << f.valid_bits
              << ",\"block_align\":" << f.block_align << ",\"bytes_per_second\":" << f.bytes_per_second
              << ",\"channel_mask\":\"" << hex32(f.channel_mask) << "\",\"channel_mask_known\":"
              << (f.channel_mask_known ? "true" : "false") << "},\n"
              << "  \"endpoint_buffer_frames\":" << r.endpoint_buffer_frames
              << ",\"default_period_100ns\":" << r.default_period_100ns
              << ",\"minimum_period_100ns\":" << r.minimum_period_100ns << ",\n"
              << "  \"packets\":" << r.packets << ",\"frames\":" << r.frames
              << ",\"silent_packets\":" << r.silent_packets << ",\"silent_frames\":" << r.silent_frames
              << ",\"discontinuity_packets\":" << r.discontinuity_packets
              << ",\"initial_discontinuity_packets\":" << r.initial_discontinuity_packets
              << ",\"timestamp_error_packets\":" << r.timestamp_error_packets << ",\n"
              << "  \"empty_polls\":" << r.empty_polls << ",\"drain_limit_hits\":" << r.drain_limit_hits
              << ",\"timestamp_pairs_capacity\":" << max_pairs << ",\"timestamp_pairs_omitted\":" << r.pairs_omitted
              << ",\n  \"timestamp_pairs\":[";
    for (std::size_t i = 0; i < r.pair_count; ++i) {
        if (i) std::cout << ',';
        std::cout << '\n' << "    ";
        write_pair(r.pairs[i]);
    }
    std::cout << "\n  ],\"last_packet\":";
    if (r.packets) write_pair(r.last_packet);
    else std::cout << "null";
    std::cout << "\n}\n";
}

void help()
{
    std::cout << "avsync-wasapi-probe --loopback [--seconds N]\n"
                 "  --seconds N  Whole seconds from 1 to 30; default 5.\n"
                 "  --help       Show help without opening an audio endpoint.\n"
                 "Explicit loopback opt-in is required. Uses the default render/console\n"
                 "endpoint's shared mix format; discards all PCM without inspecting it.\n"
                 "Reports JSON format, counters and bounded timestamp metadata only.\n"
                 "No files, network, endpoint changes, device IDs/names, or microphone.\n"
                 "Packets are not proof of audible signal, quality or end-to-end sync.\n";
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 1 || (argc == 2 && std::string_view(argv[1]) == "--help")) {
        help();
        return 0;
    }
    unsigned seconds = 5;
    bool loopback = false, duration_seen = false;
    bool valid = true;
    for (int i = 1; i < argc && valid; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--loopback" && !loopback) {
            loopback = true;
        } else if (arg == "--seconds" && !duration_seen && i + 1 < argc) {
            duration_seen = true;
            const std::string_view value(argv[++i]);
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            valid = parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size() &&
                    seconds >= 1 && seconds <= 30;
        } else {
            valid = false;
        }
    }
    if (!valid || !loopback) {
        std::cout << "{\"schema\":1,\"status\":\"error\",\"stage\":\"arguments\","
                     "\"message\":\"Use --loopback [--seconds N], with N an integer from 1 to 30; or --help.\"}\n";
        return 2;
    }
    try {
        const auto result = observe_loopback(seconds);
        write_result(result);
        return result.packets ? 0 : 3;
    } catch (const ProbeError &error) {
        std::cout << "{\"schema\":1,\"status\":\"error\",\"stage\":\"" << error.stage
                  << "\",\"hresult\":\"" << hex32(static_cast<std::uint32_t>(error.code)) << "\"}\n";
        return 1;
    } catch (...) {
        std::cout << "{\"schema\":1,\"status\":\"error\",\"stage\":\"unexpected_exception\"}\n";
        return 1;
    }
}
