// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>

namespace avsync::ipc {

// Linux-only IPC. All timestamps use CLOCK_MONOTONIC nanoseconds.
// One Reader instance is single-threaded; create one per consuming thread.
// Constructors/reconnect/destructors perform I/O: never call them in OBS callbacks.
struct Config {
    std::uint32_t width{640}, height{360}, fps{60};
    std::uint32_t video_capacity{128}, audio_capacity{224};
    std::uint32_t audio_frames{480}, audio_rate{48000};
};

enum class ReadResult { ok, empty, busy, disconnected, invalid, buffer_too_small, stale };
enum class WriteResult { ok, busy, disconnected, invalid };
enum class AllocationPolicy { sparse, reserve_and_prefault };
enum class ReplacementPolicy { atomic_replace, retire_previous };

// Maximum explicit audio handoff lead. Long intentional delays remain in the ring.
inline constexpr std::int64_t max_audio_lookahead_ns = 100000000;

struct FrameInfo {
    std::uint64_t generation{}, sequence{};
    std::int64_t capture_ns{}, presentation_ns{};
    std::uint32_t frames{}, bytes{};
};

struct Status {
    std::uint64_t generation{};
    std::int64_t epoch_ns{}, heartbeat_ns{};
    std::uint64_t video_sequence{}, audio_sequences[2]{};
    std::uint64_t skipped_video{}, skipped_audio[2]{}; // This Reader's local skips.
    bool online{}, owner_died{};
};

std::int64_t monotonic_ns() noexcept;
std::size_t video_bytes(const Config& config) noexcept;
std::size_t audio_samples(const Config& config, unsigned stream) noexcept;
bool validate_config(const Config& config, std::string& error) noexcept;
const char* result_name(ReadResult result) noexcept;

class Writer {
public:
    // Parent directory must already exist, be user-owned and not group/world writable.
    // A private .lock sidecar holds a singleton flock. The mapping is atomically replaced.
    // sparse preserves the original lazy allocation. reserve_and_prefault reserves
    // the complete new file with posix_fallocate before any mmap access, then
    // write-touches each page before initialization/publication. Reservation or
    // setup failure leaves valid()==false, preserves an existing published mapping,
    // and removes only this attempt's temporary file. No sparse fallback is used.
    // This is initialization work, not memory locking: pages can later be reclaimed
    // or swapped, so it does not guarantee zero future faults or real-time latency.
    // retire_previous is an explicit process-restart operation: after acquiring
    // the unchanged singleton sidecar, validate a protocol-v2 predecessor's own
    // inode and sidecar inode bindings, trylock its mutex, and mark it offline
    // BEFORE publishing the successor. Busy/unsafe/unrecognized predecessors are
    // refused; heartbeat expiry is never takeover authority. A missing sidecar
    // beside an existing mapping is refused. No v1 conversion is attempted.
    // Allocation/setup failure before retirement preserves the predecessor; a
    // final rename failure after retirement deliberately leaves it offline.
    // This assumes cooperative same-user ownership; it is not a hostile-owner
    // sandbox. Never unlink/replace the singleton sidecar while a writer lives.
    // Protocol v2 requires matching rebuilt producers/readers/OBS adapters.
    explicit Writer(std::string path, const Config& config,
                    AllocationPolicy allocation = AllocationPolicy::sparse,
                    ReplacementPolicy replacement = ReplacementPolicy::atomic_replace);
    ~Writer();
    Writer(const Writer&) = delete;
    Writer& operator=(const Writer&) = delete;
    bool valid() const noexcept;
    const std::string& error() const noexcept;
    std::uint64_t generation() const noexcept;
    std::int64_t epoch_ns() const noexcept;
    bool publish_video(std::span<const std::uint8_t> nv12, std::int64_t capture_ns,
                       std::int64_t presentation_ns) noexcept;
    // Worker-thread nonblocking variants. A busy reader must not stall capture.
    WriteResult try_publish_video(std::span<const std::uint8_t> nv12, std::int64_t capture_ns,
                                  std::int64_t presentation_ns) noexcept;
    WriteResult try_heartbeat() noexcept;
    // stream 0: interleaved stereo float PCM; stream 1: mono float PCM.
    bool publish_audio(unsigned stream, std::span<const float> samples,
                       std::int64_t capture_ns, std::int64_t presentation_ns) noexcept;
    WriteResult try_publish_audio(unsigned stream, std::span<const float> samples,
                       std::int64_t capture_ns, std::int64_t presentation_ns) noexcept;
    bool heartbeat() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

class Reader {
public:
    explicit Reader(std::string path);
    ~Reader();
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;
    bool reconnect(); // Explicit control/worker-thread operation; resets cursors.
    bool valid() const noexcept;
    const std::string& error() const noexcept;
    const Config& config() const noexcept;
    std::uint64_t generation() const noexcept;
    // Callback-safe: trylock only, no allocation, opening, logging, waiting or sleeping.
    // Video is tightly packed NV12 (Y followed by interleaved UV), newest due frame.
    // Never returns a future frame or repeats an already consumed frame.
    ReadResult read_latest_due_video(std::int64_t now_ns, std::span<std::uint8_t> dst,
                                    FrameInfo& info,
                                    std::int64_t max_lateness_ns = 200000000) noexcept;
    // Oldest unconsumed due audio, dropping blocks older than max_lateness_ns.
    // A stalled reader cannot replay an unbounded backlog. Negative max age is invalid.
    ReadResult read_next_due_audio(unsigned stream, std::int64_t now_ns,
                                  std::span<float> dst, FrameInfo& info,
                                  std::int64_t max_lateness_ns = 100000000) noexcept;
    // Oldest unconsumed audio with presentation <= now + max_future_ns.
    // max_future_ns must be in [0, max_audio_lookahead_ns]. Heartbeat expiry and
    // lateness use actual now_ns, not that future deadline. Timestamps are unchanged.
    // This is a small explicit handoff allowance, not a new/rebased source clock.
    ReadResult read_next_audio(unsigned stream, std::int64_t now_ns,
                               std::int64_t max_future_ns, std::span<float> dst,
                               FrameInfo& info,
                               std::int64_t max_lateness_ns = 100000000) noexcept;
    ReadResult poll_status(std::int64_t now_ns, Status& status) noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace avsync::ipc
