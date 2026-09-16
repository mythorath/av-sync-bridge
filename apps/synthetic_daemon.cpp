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
#include <thread>
#include <vector>

namespace {
volatile std::sig_atomic_t stop_requested = 0;
void stop_handler(int) { stop_requested = 1; }
bool number(const char* text, std::uint32_t& value) {
    std::string s(text); auto [p, e] = std::from_chars(s.data(), s.data() + s.size(), value);
    return e == std::errc{} && p == s.data() + s.size();
}
std::string default_path() {
    if (const auto* runtime = std::getenv("XDG_RUNTIME_DIR"); runtime && *runtime)
        return std::string(runtime) + "/av-sync-bridge.ipc";
    return {};
}
void help() {
    std::cout << "avsync-synthetic [--path FILE] [--duration SECONDS] [--delay-ms MS]\n"
                 " [--width EVEN] [--height EVEN] [--fps 1..120]\n"
                 " [--restart-fixture 1|2] (explicit finite predecessor/successor test; role 2 retires prior IPC)\n"
                 "Synthetic only: NV12 video + stereo/mono float PCM; no devices or network.\n"
                 "Defaults: 640x360, 60 fps, 48000 Hz, 480 samples/block, 2000 ms delay,\n"
                 "10 seconds capture followed by delay drain. --duration 0 runs until stopped.\n"
                 "Default path: $XDG_RUNTIME_DIR/av-sync-bridge.ipc. Without that environment\n"
                 "variable, --path is required. Parent directory must be private and user-owned.\n";
}
}

int main(int argc, char** argv) {
    avsync::ipc::Config config;
    std::string path;
    std::uint32_t duration = 10, delay_ms = 2000, fixture_id = 0;
    bool fixture_selected = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") { help(); return 0; }
        if (i + 1 == argc) { std::cerr << "Missing option value\n"; return 2; }
        const char* value = argv[++i];
        if (arg == "--path") { path = value; continue; }
        if (arg == "--restart-fixture") {
            if (fixture_selected) { std::cerr << "Duplicate restart fixture\n"; return 2; }
            fixture_selected = true;
        }
        auto* out = arg == "--duration" ? &duration : arg == "--delay-ms" ? &delay_ms :
                    arg == "--width" ? &config.width : arg == "--height" ? &config.height :
                    arg == "--fps" ? &config.fps : arg == "--restart-fixture" ? &fixture_id : nullptr;
        if (!out || !number(value, *out)) { std::cerr << "Unknown option or invalid integer\n"; return 2; }
    }
    if (duration > 3600 || delay_ms > 10000 || config.fps == 0 || config.fps > 120) {
        std::cerr << "Duration limit 3600 s, delay limit 10000 ms, fps limit 120\n"; return 2;
    }
    // Explicit bounded fixture, never an implicit change to the baseline pattern.
    // Its analyzer requires this exact format and complete successor schedule.
    if (fixture_selected && (fixture_id < 1 || fixture_id > 2 || path.empty() ||
        duration < (fixture_id == 1 ? 8u : 16u) || duration > 90 || delay_ms != 2000 ||
        config.fps != 60 || config.width != 640 || config.height != 360)) {
        std::cerr << "Restart fixture requires role 1|2, explicit path, 640x360/60, 2000 ms delay, "
                     "and finite duration 8..90 (predecessor) or 16..90 (successor)\n";
        return 2;
    }
    config.video_capacity = (delay_ms * config.fps + 999) / 1000 + 8;
    config.audio_capacity = delay_ms / 10 + 24;
    std::string error;
    if (!avsync::ipc::validate_config(config, error)) { std::cerr << error << '\n'; return 2; }
    if (path.empty()) path = default_path();
    if (path.empty()) {
        std::cerr << "XDG_RUNTIME_DIR is unset or empty; specify --path in an existing private, user-owned directory\n";
        return 2;
    }
    avsync::ipc::Writer writer(path, config, avsync::ipc::AllocationPolicy::sparse,
        fixture_id == 2 ? avsync::ipc::ReplacementPolicy::retire_previous :
                          avsync::ipc::ReplacementPolicy::atomic_replace);
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
    bool queued_barrier = false;
    std::cout << "SYNTHETIC generation=" << writer.generation() << " epoch_ns=" << epoch
              << " size=" << config.width << 'x' << config.height << " fps=" << config.fps
              << " delay_ms=" << delay_ms << " fixture_id=" << fixture_id
              << " ready_ns=" << avsync::ipc::monotonic_ns() << " path=" << path << std::endl;
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
            const bool cyan = fixture_id == 2 && event;
            std::fill(video.begin(), video.begin() + static_cast<std::ptrdiff_t>(ysize), event ? (cyan ? 188 : 235) : 16);
            // BT.709 limited-range cyan distinguishes the successor after encoding.
            // Idle frames stay black/neutral; baseline and predecessor stay white.
            for (std::size_t pixel = ysize; pixel < video.size(); pixel += 2) {
                video[pixel] = cyan ? 154 : 128;
                video[pixel + 1] = cyan ? 16 : 128;
            }
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
                        const auto frequency = 660 + event * 220 + (fixture_id == 2 ? 2200 : 0);
                        value = static_cast<float>(0.20 * envelope * std::sin(6.283185307179586 * frequency * double(delta) / 48000.0));
                    }
                }
                stereo[sample * 2] = stereo[sample * 2 + 1] = value;
                mono[sample] = value * 0.5F;
            }
            if (!writer.publish_audio(0, stereo, at, at + delay_ns) ||
                !writer.publish_audio(1, mono, at, at + delay_ns)) return 1;
            ++aindex;
        }
        if (fixture_id == 1 && !queued_barrier &&
            vindex >= static_cast<std::uint64_t>(event_frames[3] + std::max(1U, config.fps / 10)) &&
            aindex * config.audio_frames >= static_cast<std::uint64_t>(event_samples[3] + 1920)) {
            queued_barrier = true;
            // A1-A3 are already due; A4 is fully captured but still almost two
            // seconds in the future. The runner must replace before it is due.
            std::cout << "RESTART_QUEUED fixture_id=1 marker=4 generation=" << writer.generation()
                      << " barrier_ns=" << avsync::ipc::monotonic_ns()
                      << " presentation_start_ns=" << epoch + event_frames[3] * 1000000000 / config.fps + delay_ns
                      << " video_frames=" << vindex << " audio_blocks=" << aindex << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    std::cout << "Stopped synthetic producer: video=" << vindex << " audio_blocks=" << aindex
              << ". Not a hardware or OBS end-to-end test.\n";
    return 0;
}
