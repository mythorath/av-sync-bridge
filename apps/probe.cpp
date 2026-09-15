// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"
#include <algorithm>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

int main(int argc, char** argv) {
    std::string path;
    unsigned duration = 5, delay_ms = 2000;
    bool verify = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        if (arg == "--help") {
            std::cout << "avsync-probe --path FILE [--duration SECONDS] [--verify] [--delay-ms MS]\n"
                         "Reads due IPC media only; --verify checks synthetic timestamp delay,\n"
                         "monotonic sequence order, and finite receipt of all three streams.\n"
                         "This is metadata/IPC validation, NOT encoded A/V or hardware validation.\n";
            return 0;
        }
        if (arg == "--verify") { verify = true; continue; }
        if (i + 1 == argc) return 2;
        std::string value(argv[++i]);
        if (arg == "--path") path = value;
        else {
            auto* out = arg == "--duration" ? &duration : arg == "--delay-ms" ? &delay_ms : nullptr;
            if (!out) return 2;
            auto [p, e] = std::from_chars(value.data(), value.data() + value.size(), *out);
            if (e != std::errc{} || p != value.data() + value.size()) return 2;
        }
    }
    if (path.empty() || duration == 0 || duration > 3600 || delay_ms > 10000) return 2;
    avsync::ipc::Reader reader(path);
    if (!reader.valid()) { std::cerr << reader.error() << '\n'; return 1; }
    std::vector<std::uint8_t> video(avsync::ipc::video_bytes(reader.config()));
    std::vector<float> audio(avsync::ipc::audio_samples(reader.config(), 0));
    std::uint64_t received[3]{}, sequences[3]{};
    std::uint64_t busy = 0, stale = 0, disconnected = 0;
    std::int64_t max_lateness = 0;
    bool failed = false;
    const auto begin = avsync::ipc::monotonic_ns();
    auto next_status = begin;
    auto accept = [&](unsigned stream, avsync::ipc::ReadResult result, const avsync::ipc::FrameInfo& info, std::int64_t now) {
        using avsync::ipc::ReadResult;
        if (result == ReadResult::busy) ++busy;
        else if (result == ReadResult::stale) ++stale;
        else if (result == ReadResult::disconnected) ++disconnected;
        else if (result == ReadResult::invalid || result == ReadResult::buffer_too_small) failed = true;
        else if (result == ReadResult::ok) {
            if (info.sequence <= sequences[stream] || info.presentation_ns > now) failed = true;
            if (verify && info.presentation_ns - info.capture_ns != std::int64_t(delay_ms) * 1000000) failed = true;
            max_lateness = std::max(max_lateness, now - info.presentation_ns);
            sequences[stream] = info.sequence; ++received[stream];
        }
    };
    while (avsync::ipc::monotonic_ns() - begin < std::int64_t(duration) * 1000000000) {
        const auto now = avsync::ipc::monotonic_ns();
        avsync::ipc::FrameInfo info;
        auto result = reader.read_latest_due_video(now, video, info); accept(0, result, info, now);
        for (unsigned stream = 0; stream < 2; ++stream) {
            for (unsigned drain = 0; drain < 16; ++drain) {
                result = reader.read_next_due_audio(stream, now, audio, info);
                accept(stream + 1, result, info, now);
                if (result != avsync::ipc::ReadResult::ok) break;
            }
        }
        if (now >= next_status) {
            avsync::ipc::Status status;
            result = reader.poll_status(now, status);
            std::cout << "state=" << avsync::ipc::result_name(result) << " generation=" << reader.generation()
                      << " received=" << received[0] << ',' << received[1] << ',' << received[2]
                      << " max_delivery_late_ms=" << double(max_lateness) / 1e6 << '\n';
            next_status = now + 1000000000;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    if (verify && (!received[0] || !received[1] || !received[2])) failed = true;
    std::cout << "IPC " << (failed ? "FAIL" : "PASS") << " counts=" << received[0] << ',' << received[1] << ',' << received[2]
              << " busy=" << busy << " stale=" << stale << " disconnected_polls=" << disconnected
              << "; metadata-only, not end-to-end validation\n";
    return failed ? 1 : 0;
}
