// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifdef AVSYNC_IPC_TEST_HOOKS
namespace avsync::ipc::testing {
[[noreturn]] void abandon_mutex(const char*, int, int);
bool set_sequence(const char*, unsigned, std::uint64_t);
}
#endif

namespace {
using namespace avsync::ipc;
void require(bool condition, const char* message) { if (!condition) throw std::runtime_error(message); }
struct TempDir {
    std::string path;
    std::vector<std::string> files;
    TempDir() {
        char pattern[] = "/tmp/avsync-ipc-test-XXXXXX";
        auto* p = mkdtemp(pattern); if (!p) throw std::runtime_error("mkdtemp"); path = p;
    }
    std::string file(const std::string& name) {
        auto p = path + '/' + name; files.push_back(p); files.push_back(p + ".lock"); return p;
    }
    ~TempDir() { for (const auto& file : files) unlink(file.c_str()); rmdir(path.c_str()); }
};
Config small() { Config c; c.width = 16; c.height = 16; c.video_capacity = 4; c.audio_capacity = 4; return c; }

void no_temporary_files(const TempDir& dir) {
    auto* directory = opendir(dir.path.c_str()); require(directory != nullptr, "inspect allocation cleanup");
    bool clean = true; unsigned entries = 0;
    errno = 0;
    while (const auto* entry = readdir(directory)) {
        if (++entries > 64 || std::strstr(entry->d_name, ".tmp.") != nullptr) { clean = false; break; }
    }
    const int read_error = errno; closedir(directory);
    require(clean && read_error == 0, "allocation attempt must not leave temporary mappings");
}

// The child alone has a zero-byte file-size limit. No disk/tmpfs is filled,
// no device is opened, and the parent's limits and signal dispositions persist.
void reject_reservation_with_child_limit(const std::string& path, const Config& config) {
    const auto child = fork(); require(child >= 0, "fork reservation failure fixture");
    if (!child) {
        rlimit limit{};
        if (signal(SIGALRM, SIG_DFL) == SIG_ERR) _exit(80);
        alarm(5); // A regression must not leave the parent waiting indefinitely.
        if (getrlimit(RLIMIT_FSIZE, &limit) != 0 || signal(SIGXFSZ, SIG_IGN) == SIG_ERR) _exit(81);
        limit.rlim_cur = 0;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) _exit(82);
        bool rejected = false;
        try {
            Writer writer(path, config, AllocationPolicy::reserve_and_prefault);
            rejected = !writer.valid() && writer.error() == "cannot reserve full IPC mapping";
        } catch (...) { _exit(83); }
        _exit(rejected ? 0 : 84); // Writer has already released mappings and flock.
    }
    int code{}; pid_t waited;
    do { waited = waitpid(child, &code, 0); } while (waited < 0 && errno == EINTR);
    require(waited == child && WIFEXITED(code) && WEXITSTATUS(code) == 0,
            "reservation failure must be explicit, not SIGBUS/SIGXFSZ or sparse fallback");
}

void reserved_allocation_and_cleanup(TempDir& dir) {
    const auto config = small(); const auto success = dir.file("reserved");
    struct stat previous{};
    std::uint64_t old_generation{};
    {
        Writer writer(success, config, AllocationPolicy::reserve_and_prefault);
        require(writer.valid(), "reserved and prefaulted writer create");
        require(stat(success.c_str(), &previous) == 0 && previous.st_size > 0 &&
                (previous.st_mode & 0777) == 0600, "reserved private mapping");
        old_generation = writer.generation();
        Reader reader(success); require(reader.valid(), "reserved mapping remains protocol-compatible");
        std::vector<std::uint8_t> input(video_bytes(config), 0xA7), output(input.size());
        std::vector<float> audio(audio_samples(config, 0), 0.25F), pcm(audio.size());
        const auto t = monotonic_ns(); FrameInfo info;
        for (std::uint64_t n = 0; n < config.video_capacity + 2U; ++n) {
            const auto capture = t + static_cast<std::int64_t>(n);
            require(writer.try_publish_video(input, capture, capture) == WriteResult::ok,
                    "reserved video publish across ring wrap");
            require(reader.read_latest_due_video(capture, output, info) == ReadResult::ok &&
                    input == output && info.sequence == n + 1, "prefault did not corrupt published pixels");
        }
        require(writer.publish_audio(0, audio, t, t) &&
                reader.read_next_due_audio(0, t, pcm, info) == ReadResult::ok && pcm == audio,
                "reserved allocation preserves audio layout");
    }
    no_temporary_files(dir);

    // A failed replacement cannot remove or alter the previously published inode.
    reject_reservation_with_child_limit(success, config);
    struct stat after{};
    require(stat(success.c_str(), &after) == 0 && after.st_ino == previous.st_ino &&
            after.st_dev == previous.st_dev && after.st_size == previous.st_size,
            "failed reservation preserves existing published mapping");
    Reader retained(success);
    require(retained.valid() && retained.generation() == old_generation,
            "failed replacement preserves existing header and generation");
    no_temporary_files(dir);

    const auto failure = dir.file("reserve-failure");
    reject_reservation_with_child_limit(failure, config);
    require(lstat(failure.c_str(), &after) != 0 && errno == ENOENT,
            "failed reservation must not publish a partial mapping");
    no_temporary_files(dir);
    // Parent limit was not changed, and the failed child no longer owns the lock.
    Writer retry(failure, config, AllocationPolicy::reserve_and_prefault);
    require(retry.valid(), "reservation retry after failure releases singleton lock");
    const auto invalid = dir.file("invalid-allocation");
    Writer rejected(invalid, config, static_cast<AllocationPolicy>(99));
    require(!rejected.valid() && lstat(invalid.c_str(), &after) != 0 && errno == ENOENT,
            "invalid allocation policy has no published file");
}

void scheduling_and_bounds(TempDir& dir) {
    const auto path = dir.file("schedule"); auto c = small(); Writer writer(path, c);
    require(writer.valid(), "writer create"); Reader reader(path); require(reader.valid(), "reader create");
    std::vector<std::uint8_t> input(video_bytes(c), 123), output(video_bytes(c));
    std::vector<float> samples(audio_samples(c, 0), 0.25F), pcm(samples.size());
    const auto t = monotonic_ns(); FrameInfo info;
    require(writer.publish_video(input, t, t + 100000000), "video publish");
    require(reader.read_latest_due_video(t, output, info) == ReadResult::empty, "must not release future video");
    require(reader.read_latest_due_video(t + 100000000, output, info) == ReadResult::ok, "due video");
    require(output == input && info.capture_ns == t && info.presentation_ns == t + 100000000, "video payload/timestamps");
    require(reader.read_latest_due_video(t + 100000000, output, info) == ReadResult::empty, "no repeated video");
    require(reader.read_latest_due_video(t, std::span<std::uint8_t>(output.data(), 1), info) == ReadResult::buffer_too_small, "small video span");
    require(!writer.publish_video(std::span<const std::uint8_t>(input.data(), 1), t, t), "reject short video");
    require(!writer.publish_video(input, -1, t), "reject negative capture");
    require(!writer.publish_video(input, t + 100, t), "reject reversed timestamp");
    require(writer.try_publish_video(input, t + 1, t + 100000001) == WriteResult::ok, "nonblocking video publish");
    require(writer.try_publish_video(input, -1, t) == WriteResult::invalid, "nonblocking invalid timestamp");
    require(writer.try_publish_video(std::span<const std::uint8_t>(input.data(), 1), t, t) == WriteResult::invalid,
            "nonblocking invalid size");
    require(writer.try_heartbeat() == WriteResult::ok, "nonblocking heartbeat");
    require(reader.read_latest_due_video(t + 100000001, output, info) == ReadResult::ok && info.sequence == 2,
            "nonblocking preserves timestamps and sequence");
    require(writer.publish_audio(0, samples, t, t + 100000000), "audio publish");
    require(reader.read_next_due_audio(0, t, pcm, info) == ReadResult::empty, "must not release future audio");
    require(reader.read_next_due_audio(0, t + 100000000, pcm, info) == ReadResult::ok && samples == pcm, "audio payload");
    require(reader.read_next_due_audio(2, t, pcm, info) == ReadResult::invalid, "invalid audio stream");
    require(reader.read_next_due_audio(0, t, pcm, info, -1) == ReadResult::invalid, "negative stale limit");
    for (unsigned i = 1; i <= 10; ++i) {
        require(writer.publish_video(input, t + i * 10000000, t + 100000000 + i * 10000000), "ring video publish");
        require(writer.publish_audio(0, samples, t + i * 10000000, t + 100000000 + i * 10000000), "ring audio publish");
    }
    require(reader.read_latest_due_video(t + 200000000, output, info) == ReadResult::ok && info.sequence == 12, "newest due after overwrite");
    require(reader.read_next_due_audio(0, t + 500000000, pcm, info, 1000000) == ReadResult::stale, "stale audio dropped");
    require(reader.read_next_due_audio(0, t + 500000000, pcm, info) == ReadResult::empty, "no stale catch-up");
    Status status;
    require(reader.poll_status(t + 500000000, status) == ReadResult::ok && status.skipped_video == 9 && status.skipped_audio[0] == 10, "skip counters");
    require(reader.poll_status(t + 3000000000LL, status) == ReadResult::disconnected, "heartbeat expiry");
}

void bounded_audio_handoff(TempDir& dir) {
    const auto c = small(); const auto path = dir.file("handoff"); Writer writer(path, c); Reader reader(path);
    require(writer.valid() && reader.valid(), "handoff fixture");
    std::vector<float> samples(audio_samples(c, 0), 0.125F), output(samples.size()); FrameInfo info;
    const auto t = monotonic_ns() + 50000000;
    require(writer.publish_audio(0, samples, t - 20000000, t - 15000000), "past handoff publish");
    require(reader.read_next_audio(0, t, 40000000, output, info, 20000000) == ReadResult::ok,
            "lateness must use real now, not the future deadline");
    require(output == samples && info.sequence == 1, "past handoff payload");
    require(writer.publish_audio(0, samples, t, t + 40000001), "future handoff publish");
    require(reader.read_next_audio(0, t, 40000000, output, info) == ReadResult::empty,
            "one nanosecond beyond handoff window stays queued");
    require(reader.read_next_audio(0, t, -1, output, info) == ReadResult::invalid, "negative handoff lead rejected");
    require(reader.read_next_audio(0, t, max_audio_lookahead_ns + 1, output, info) == ReadResult::invalid,
            "excess handoff lead rejected");
    require(reader.read_next_audio(0, -1, 0, output, info) == ReadResult::invalid, "negative real time rejected");
    require(reader.read_next_audio(0, t, 40000000, output, info, -1) == ReadResult::invalid,
            "negative handoff stale limit rejected");
    require(reader.read_next_audio(2, t, 40000000, output, info) == ReadResult::invalid, "invalid handoff stream");
    require(reader.read_next_audio(0, t, 40000000, std::span<float>(output.data(), 1), info) == ReadResult::buffer_too_small,
            "short handoff destination rejected");
    require(reader.read_next_due_audio(0, t + 1, output, info) == ReadResult::empty, "due-only API remains due-only");
    require(reader.read_next_audio(0, t + 1, 40000000, output, info) == ReadResult::ok && info.sequence == 2 &&
            info.capture_ns == t && info.presentation_ns == t + 40000001,
            "inclusive handoff boundary and invalid reads preserve cursor/timestamps");
    require(reader.read_next_audio(0, t + 1, 40000000, output, info) == ReadResult::empty, "no repeated handoff block");
    require(writer.publish_audio(0, samples, t + 1, t + 100000001), "maximum lead publish");
    require(reader.read_next_audio(0, t + 1, max_audio_lookahead_ns, output, info) == ReadResult::ok && info.sequence == 3,
            "maximum bounded lead accepted");
    require(writer.publish_audio(0, samples, t + 2, std::numeric_limits<std::int64_t>::max()), "maximum timestamp publish");
    require(reader.read_next_audio(0, t + 2, max_audio_lookahead_ns, output, info) == ReadResult::empty,
            "distant future timestamp remains queued without overflow");
    require(reader.read_next_audio(0, std::numeric_limits<std::int64_t>::max(), max_audio_lookahead_ns, output, info)
            == ReadResult::disconnected, "extreme current time expires heartbeat without deadline overflow");

    const auto mixed_path = dir.file("handoff-stale"); Writer mixed_writer(mixed_path, c); Reader mixed(mixed_path);
    require(mixed_writer.publish_audio(0, samples, t, t), "mixed stale publish");
    require(mixed_writer.publish_audio(0, samples, t + 30000000, t + 30000000), "mixed future publish");
    require(mixed.read_next_audio(0, t + 20000000, 10000000, output, info, 10000000) == ReadResult::ok && info.sequence == 2,
            "drop stale block then return oldest eligible future block");
    Status status;
    require(mixed.poll_status(t + 20000000, status) == ReadResult::ok && status.skipped_audio[0] == 1, "handoff skip accounting");

    const auto heartbeat_path = dir.file("handoff-heartbeat"); Writer heartbeat_writer(heartbeat_path, c); Reader heartbeat_reader(heartbeat_path);
    const auto start = monotonic_ns();
    require(heartbeat_writer.publish_audio(0, samples, start, start + 2030000000LL), "heartbeat handoff publish");
    require(heartbeat_reader.poll_status(monotonic_ns(), status) == ReadResult::ok, "heartbeat handoff snapshot");
    require(heartbeat_reader.read_next_audio(0, status.heartbeat_ns + 2000000000LL, 40000000, output, info,
                                            std::numeric_limits<std::int64_t>::max()) == ReadResult::ok,
            "future handoff must not prematurely expire real heartbeat");
    require(heartbeat_reader.read_next_audio(0, status.heartbeat_ns + 2000000001LL, 40000000, output, info)
            == ReadResult::disconnected, "real heartbeat boundary still enforced");
}

void handoff_restart_phase(TempDir& dir) {
    const auto c = small(); const auto path = dir.file("handoff-restart");
    auto writer = std::make_unique<Writer>(path, c); Reader reader(path);
    require(writer->valid() && reader.valid(), "handoff restart fixture");
    std::vector<float> samples(audio_samples(c, 1), 0.0625F), output(samples.size()); FrameInfo info;
    // Deliberately not aligned with a global 10 ms boundary. Readers must not round/rebase it.
    auto capture = (monotonic_ns() / 10000000) * 10000000 + 1234567;
    require(writer->publish_audio(1, samples, capture, capture + 100000000), "first phase publish");
    require(reader.read_next_audio(1, capture + 60000000, 40000000, output, info) == ReadResult::ok, "first phase read");
    const auto old_generation = info.generation;
    writer.reset();
    writer = std::make_unique<Writer>(path, c); require(writer->valid(), "new handoff generation");
    capture = (monotonic_ns() / 10000000) * 10000000 + 7654321;
    require(writer->publish_audio(1, samples, capture, capture + 100000000), "restarted phase publish");
    require(reader.read_next_audio(1, capture + 60000000, 40000000, output, info) == ReadResult::disconnected,
            "old generation cannot release replacement media");
    require(reader.reconnect(), "handoff explicit reconnect");
    require(reader.read_next_audio(1, capture + 60000000, 40000000, output, info) == ReadResult::ok &&
            info.generation != old_generation && info.generation == writer->generation() && info.sequence == 1 &&
            info.capture_ns == capture && info.presentation_ns == capture + 100000000 && output == samples,
            "new generation resets cursor but preserves fractional capture/presentation phase");
}

void restart_and_security(TempDir& dir) {
    auto c = small(); auto path = dir.file("restart");
    auto writer = std::make_unique<Writer>(path, c); require(writer->valid(), "first producer");
    const auto generation = writer->generation(); Reader reader(path); require(reader.valid(), "restart reader");
    Writer duplicate(path, c); require(!duplicate.valid(), "singleton producer lock");
    struct stat st{}; require(stat(path.c_str(), &st) == 0 && (st.st_mode & 0777) == 0600, "mapping mode0600");
    writer.reset(); Status status;
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected, "clean disconnect");
    writer = std::make_unique<Writer>(path, c); require(writer->valid(), "restart producer");
    require(writer->generation() != generation, "new generation");
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected, "old mapping remains safe/stale");
    require(reader.reconnect() && reader.generation() == writer->generation(), "explicit remap new generation");
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::ok, "new producer online");
    auto link = dir.file("symlink"); require(symlink(path.c_str(), link.c_str()) == 0, "create symlink fixture");
    Reader unsafe_reader(link); Writer unsafe_writer(link, c);
    require(!unsafe_reader.valid() && !unsafe_writer.valid(), "reject symlink");
    chmod(path.c_str(), 0644); Reader public_reader(path); require(!public_reader.valid(), "reject public mapping"); chmod(path.c_str(), 0600);
    Writer unsafe_parent("/tmp/avsync-no-public-parent", c); require(!unsafe_parent.valid(), "reject public parent");
    auto invalid = c; invalid.width = 3842; std::string error;
    require(!validate_config(invalid, error), "reject oversize video");
    invalid = c; invalid.width = 15; require(!validate_config(invalid, error), "reject odd NV12");
    invalid = c; invalid.video_capacity = 0; require(!validate_config(invalid, error), "reject zero capacity");
    invalid = c; invalid.width = 3840; invalid.height = 2160; invalid.video_capacity = 2400;
    require(!validate_config(invalid, error), "reject memory budget overflow");
    const auto bad = dir.file("corrupt");
    int fd = open(bad.c_str(), O_RDWR | O_CREAT | O_EXCL, 0600); require(fd >= 0, "corrupt fixture");
    require(ftruncate(fd, 4096) == 0, "size corrupt fixture"); close(fd);
    Reader corrupt(bad); require(!corrupt.valid(), "reject bad magic");
    const auto version_path = dir.file("version");
    { Writer version_writer(version_path, c); require(version_writer.valid(), "version fixture"); }
    fd = open(version_path.c_str(), O_RDWR); std::uint32_t wrong_version = 999;
    require(pwrite(fd, &wrong_version, sizeof(wrong_version), sizeof(std::uint64_t)) == sizeof(wrong_version), "write bad version"); close(fd);
    Reader wrong(version_path); require(!wrong.valid(), "reject unknown version");
}

#ifdef AVSYNC_IPC_TEST_HOOKS
void owner_death_and_wrap(TempDir& dir) {
    auto c = small(); auto path = dir.file("death"); Writer writer(path, c); require(writer.valid(), "death writer"); Reader reader(path);
    int notification[2]; require(pipe(notification) == 0, "notification pipe");
    int release[2]; require(pipe(release) == 0, "release pipe");
    const auto child = fork(); require(child >= 0, "fork");
    if (!child) { close(notification[0]); close(release[1]); testing::abandon_mutex(path.c_str(), notification[1], release[0]); }
    close(notification[1]); close(release[0]); char marker{};
    require(read(notification[0], &marker, 1) == 1 && marker == 'R', "child owns mutex"); close(notification[0]);
    Status status;
    const auto busy_result = reader.poll_status(monotonic_ns(), status);
    std::vector<std::uint8_t> video(video_bytes(c), 12);
    const auto busy_publish = writer.try_publish_video(video, 1, 2);
    const auto busy_heartbeat = writer.try_heartbeat();
    require(write(release[1], &marker, 1) == 1, "release fault owner"); close(release[1]);
    int code{}; require(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 99, "owner exits locked");
    require(busy_result == ReadResult::busy, "reader must not wait for mutex");
    require(busy_publish == WriteResult::busy && busy_heartbeat == WriteResult::busy,
            "capture worker must not wait for an IPC reader mutex");
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected && status.owner_died, "robust recovery rejects incomplete epoch");
    require(!writer.heartbeat(), "dead epoch cannot resume");
    require(writer.try_publish_video(video, 1, 2) == WriteResult::disconnected &&
            writer.try_heartbeat() == WriteResult::disconnected, "nonblocking rejects abandoned generation");
    const auto wrap_path = dir.file("wrap"); Writer wrap_writer(wrap_path, c); Reader wrap_reader(wrap_path);
    const auto max = std::numeric_limits<std::uint64_t>::max();
    require(testing::set_sequence(wrap_path.c_str(), 1, max - c.audio_capacity), "set near-wrap counter");
    std::vector<float> pcm(audio_samples(c, 0), 0); const auto t = monotonic_ns();
    for (unsigned i = 0; i < c.audio_capacity; ++i) require(wrap_writer.publish_audio(0, pcm, t + i, t + i), "near-wrap publish");
    require(!wrap_writer.publish_audio(0, pcm, t + 10, t + 10), "no sequence wrap on publish");
    FrameInfo info;
    require(wrap_reader.read_next_due_audio(0, t + 100000000, pcm, info, 0) == ReadResult::stale, "bounded max-sequence stale scan");
    require(wrap_reader.read_next_due_audio(0, t + 100000000, pcm, info) == ReadResult::empty, "max-sequence consumed");
}
#endif
}

int main() {
    try {
        TempDir dir; reserved_allocation_and_cleanup(dir); scheduling_and_bounds(dir);
        bounded_audio_handoff(dir); handoff_restart_phase(dir); restart_and_security(dir);
#ifdef AVSYNC_IPC_TEST_HOOKS
        owner_death_and_wrap(dir);
#else
        std::cout << "SKIP robust owner-death/wrap injection: AVSYNC_IPC_TEST_HOOKS not compiled\n";
#endif
        std::cout << "PASS: IPC scheduling, payload, bounds, stale/drop, restart, security tests. Synthetic only.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
