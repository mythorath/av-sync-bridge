// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop_handler(int) { stop_requested = 1; }
bool number(const char* text, std::uint32_t& value) {
    std::string s(text); auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), value);
    return e == std::errc{} && p == s.data() + s.size();
}
std::string default_path() {
    if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR")) return std::string(runtime) + "/av-sync-synthetic.ipc";
    auto parent = std::string("/tmp/av-sync-synthetic-") + std::to_string(geteuid());
    mkdir(parent.c_str(), 0700);
    return parent + "/media.ipc";
}
void help() {
    std::cout << "avsync-synthetic --path FILE [--duration SECONDS] [--delay-ms MS]\n"
                 " [--width EVEN] [--height EVEN] [--fps 1..120]\n"
                 "Synthetic only: NV12 video + stereo/mono float PCM; no devices or network.\n"
                 "Defaults: 640x360, 60 fps, 48000 Hz, 480 samples/block, 2000 ms delay,\n"
                 "10 seconds capture followed by delay drain. --duration 0 runs until stopped.\n"
                 "Parent directory must be private and user-owned.\n";
}
}

int main(int argc, char** argv) {
    avsync::ipc::Config config;
    std::string path;
    std::uint32_t duration = 10, delay_ms = 2000;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") { help(); return 0; }
        if (i + 1 == argc) { std::cerr << "Missing option value\n"; return 2; }
        const char* value = argv[++i];
        if (arg == "--path") { path = value; continue; }
        auto* out = arg == "--duration" ? &duration : arg == "--delay-ms" ? &delay_ms :
                    arg == "--width" ? &config.width : arg == "--height" ? &config.height :
                    arg == "--fps" ? &config.fps : nullptr;
        if (!out || !number(value, *out)) { std::cerr << "Unknown option or invalid integer\n"; return 2; }
    }
    if (duration > 3600 || delay_ms > 10000 || config.fps == 0 || config.fps > 120) {
        std::cerr << "Duration limit 3600 s, delay limit 10000 ms, fps limit 120\n"; return 2;
    }
    config.video_capacity = (delay_ms * config.fps + 999) / 1000 + 8;
    config.audio_capacity = delay_ms / 10 + 24;
    std::string error;
    if (!avsync::ipc::validate_config(config, error)) { std::cerr << error << '\n'; return 2; }
    if (path.empty()) path = default_path();
    avsync::ipc::Writer writer(path, config);
    if (!writer.valid()) { std::cerr << writer.error() << '\n'; return 1; }
    std::signal(SIGINT, stop_handler); std::signal(SIGTERM, stop_handler);
    std::vector<std::uint8_t> video(avsync::ipc::video_bytes(config), 128);
    std::vector<float> stereo(config.audio_frames * 2), mono(config.audio_frames);
    const auto epoch = writer.epoch_ns();
    const auto delay_ns = std::int64_t(delay_ms) * 1000000;
    const auto duration_ns = std::int64_t(duration) * 1000000000;
    const std::array<std::int64_t, 6> event_ms{1000, 2350, 4100, 6700, 10150, 14800};
    std::array<std::int64_t, 6> event_frames{}, event_samples{};
    for (std::size_t i = 0; i < event_ms.size(); ++i) {
        event_frames[i] = (event_ms[i] * config.fps + 500) / 1000;
        event_samples[i] = event_frames[i] * 48000 / config.fps;
    }
    std::uint64_t vindex = 0, aindex = 0;
    std::cout << "SYNTHETIC generation=" << writer.generation() << " epoch_ns=" << epoch
              << " size=" << config.width << 'x' << config.height << " fps=" << config.fps
              << " delay_ms=" << delay_ms << " path=" << path << std::endl;
    while (!stop_requested) {
        const auto now = avsync::ipc::monotonic_ns();
        const auto elapsed = now - epoch;
        if (duration && elapsed >= duration_ns + delay_ns + 100000000) break;
        if (duration && elapsed >= duration_ns) {
            writer.heartbeat(); std::this_thread::sleep_for(std::chrono::milliseconds(5)); continue;
        }
        const auto vt = epoch + static_cast<std::int64_t>(vindex) * 1000000000 / config.fps;
        if (now >= vt) {
            const auto cycle_frame = static_cast<std::int64_t>(vindex % (20 * config.fps));
            unsigned event = 0;
            for (unsigned i = 0; i < event_frames.size(); ++i)
                if (cycle_frame >= event_frames[i] && cycle_frame < event_frames[i] + std::max(1U, config.fps / 10)) event = i + 1;
            const auto ysize = static_cast<std::size_t>(config.width) * config.height;
            std::fill(video.begin(), video.begin() + static_cast<std::ptrdiff_t>(ysize), event ? 235 : 16);
            // Top-left binary event code; bottom row encodes the frame index.
            for (unsigned bit = 0; bit < 6 && bit < config.width; ++bit) {
                video[bit] = (event & (1U << bit)) ? 235 : 16;
                video[ysize - config.width + bit] = (vindex & (1ULL << bit)) ? 235 : 16;
            }
            if (!writer.publish_video(video, vt, vt + delay_ns)) return 1;
            ++vindex;
        }
        const auto at = epoch + static_cast<std::int64_t>(aindex) * 10000000;
        if (now >= at) {
            for (unsigned sample = 0; sample < config.audio_frames; ++sample) {
                const auto absolute = static_cast<std::int64_t>(aindex * config.audio_frames + sample);
                const auto cycle = absolute % (20 * 48000);
                float value = 0;
                for (unsigned event = 0; event < event_samples.size(); ++event) {
                    const auto delta = cycle - event_samples[event];
                    if (delta >= 0 && delta < 1920) {
                        const auto envelope = std::min(1.0, double(delta) / 48.0) * std::min(1.0, double(1920 - delta) / 48.0);
                        value = static_cast<float>(0.20 * envelope * std::sin(6.283185307179586 * (660 + event * 220) * double(delta) / 48000.0));
                    }
                }
                stereo[sample * 2] = stereo[sample * 2 + 1] = value;
                mono[sample] = value * 0.5F;
            }
            if (!writer.publish_audio(0, stereo, at, at + delay_ns) ||
                !writer.publish_audio(1, mono, at, at + delay_ns)) return 1;
            ++aindex;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "Stopped synthetic producer: video=" << vindex << " audio_blocks=" << aindex
              << ". Not a hardware or OBS end-to-end test.\n";
    return 0;
}
