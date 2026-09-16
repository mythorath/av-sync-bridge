// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"
#include "avsync/video_publication.hpp"
#include <algorithm>
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
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#ifdef AVSYNC_IPC_TEST_HOOKS
namespace avsync::ipc::testing {
[[noreturn]] void abandon_mutex(const char*, int, int);
[[noreturn]] void briefly_hold_mutex(const char*, int, int);
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
void reject_reservation_with_child_limit(const std::string& path, const Config& config,
        ReplacementPolicy replacement = ReplacementPolicy::atomic_replace) {
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
            Writer writer(path, config, AllocationPolicy::reserve_and_prefault, replacement);
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

void explicit_process_replacement(TempDir& dir) {
    const auto config = small(); const auto path = dir.file("process-replace");
    auto writer = std::make_unique<Writer>(path, config);
    require(writer->valid(), "replacement first writer");
    Reader reader(path); const auto old_generation = reader.generation();
    {
        Writer refused(path, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
        require(!refused.valid() && writer->heartbeat(), "replacement cannot seize a live producer");
    }
    writer.reset();
    writer = std::make_unique<Writer>(path, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
    require(writer->valid() && writer->generation() != old_generation, "explicit graceful replacement");
    Status status;
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected, "graceful old mapping remains offline");
    writer.reset();

    // Actual producer subprocess dies without C++ destruction, leaving online=1
    // and due/future distinct PCM. The parent must retire that mmap immediately,
    // not wait for (or derive authority from) the heartbeat timeout.
    for (const bool abandoned_mutex : {false, true}) {
#ifndef AVSYNC_IPC_TEST_HOOKS
        if (abandoned_mutex) continue;
#endif
        const auto crashed = dir.file(abandoned_mutex ? "crashed-locked" : "crashed-unlocked");
        int notification[2], release[2];
        require(pipe(notification) == 0 && pipe(release) == 0, "replacement child pipes");
        const auto child = fork(); require(child >= 0, "replacement producer fork");
        if (!child) {
            close(notification[0]); close(release[1]); alarm(5);
            Writer producer(crashed, config);
            if (!producer.valid()) _exit(81);
            std::vector<float> old_pcm(audio_samples(config, 0), .25F);
            const auto capture = monotonic_ns();
            if (!producer.publish_audio(0, old_pcm, capture, capture + 100'000'000)) _exit(82);
#ifdef AVSYNC_IPC_TEST_HOOKS
            if (abandoned_mutex)
                testing::abandon_mutex(crashed.c_str(), notification[1], release[0]);
#endif
            const char ready = 'R';
            if (write(notification[1], &ready, 1) != 1) _exit(83);
            char wait{}; if (read(release[0], &wait, 1) != 1) _exit(85);
            _exit(84);
        }
        close(notification[1]); close(release[0]); char marker{};
        const bool notified = read(notification[0], &marker, 1) == 1 && marker == 'R';
        close(notification[0]);
        Reader old_reader(crashed);
        const auto generation = old_reader.generation();
        const auto killed = kill(child, SIGKILL);
        close(release[1]); int code{};
        const auto waited = waitpid(child, &code, 0);
        require(notified && killed == 0 && waited == child && WIFSIGNALED(code) && WTERMSIG(code) == SIGKILL,
                "hard-dead producer was reaped");
        require(old_reader.valid(), "old reader holds crashed mapping");
        if (!abandoned_mutex) {
            // A synthetic earlier 'now' proves the online bit, independently of
            // scheduler speed and the heartbeat-age gate. Failed allocation must
            // not prematurely retire the old mapping.
            require(old_reader.poll_status(0, status) == ReadResult::ok, "hard-dead unlocked writer left online state");
            reject_reservation_with_child_limit(crashed, config, ReplacementPolicy::retire_previous);
            require(old_reader.poll_status(0, status) == ReadResult::ok, "pre-retirement allocation failure preserves old state");
        }
        Writer successor(crashed, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
        require(successor.valid(), "explicit hard-death replacement");
        std::vector<float> fresh(audio_samples(config, 0), -.5F), output(fresh.size(), 7.F);
        const auto capture = monotonic_ns(); FrameInfo info;
        require(successor.publish_audio(0, fresh, capture, capture + 100'000'000), "successor distinct PCM publish");
        require(old_reader.poll_status(0, status) == ReadResult::disconnected && !status.online &&
                status.owner_died == abandoned_mutex, "old generation retired before new publication");
        require(old_reader.read_next_due_audio(0, capture + 100'000'000, output, info) == ReadResult::disconnected &&
                std::all_of(output.begin(), output.end(), [](float x) { return x == 7.F; }), "no old queued PCM is copied");
        require(old_reader.reconnect() && old_reader.generation() != generation, "reader adopts successor explicitly");
        require(old_reader.read_next_due_audio(0, capture + 100'000'000, output, info) == ReadResult::ok &&
                output == fresh && info.sequence == 1 && info.capture_ns == capture &&
                info.presentation_ns == capture + 100'000'000, "successor PCM and dates preserved without rebase");
    }
}

void replacement_security(TempDir& dir) {
    const auto config = small(); const auto fresh = dir.file("replacement-new");
    { Writer writer(fresh, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
      require(writer.valid(), "retirement policy permits a fresh path"); }
    const auto corrupted = dir.file("replacement-corrupt");
    int fd = open(corrupted.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    require(fd >= 0 && ftruncate(fd, 4096) == 0, "unrelated private-file fixture"); close(fd);
    fd = open((corrupted + ".lock").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
    require(fd >= 0, "unrelated sidecar fixture"); close(fd);
    struct stat original{}, after{};
    require(lstat(corrupted.c_str(), &original) == 0, "unrelated file identity");
    { Writer refused(corrupted, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
      require(!refused.valid(), "unknown private file cannot be retired or replaced"); }
    require(lstat(corrupted.c_str(), &after) == 0 && after.st_ino == original.st_ino && after.st_size == original.st_size,
            "unknown file retained unchanged");

    for (const auto* kind : {"missing-lock", "replaced-lock", "mapping-link", "lock-link", "public", "old-version", "copied-map"}) {
        const auto path = dir.file(std::string("replacement-") + kind);
        auto live = std::make_unique<Writer>(path, config); require(live->valid(), "unsafe replacement fixture");
        Reader old(path); const auto generation = old.generation();
        const bool hold_live = std::string(kind) == "replaced-lock";
        if (!hold_live) live.reset();
        if (std::string(kind) == "missing-lock" || hold_live) {
            require(unlink((path + ".lock").c_str()) == 0, "remove fixture singleton name");
            if (hold_live) {
                fd = open((path + ".lock").c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
                require(fd >= 0, "replacement lock inode fixture"); close(fd);
            }
        } else if (std::string(kind) == "mapping-link" || std::string(kind) == "lock-link") {
            const auto alias = dir.file(std::string(kind) + "-alias");
            require(link((std::string(kind) == "mapping-link" ? path : path + ".lock").c_str(), alias.c_str()) == 0,
                    "hardlink fixture");
        } else if (std::string(kind) == "public") {
            require(chmod(path.c_str(), 0644) == 0, "unsafe mode fixture");
        } else if (std::string(kind) == "old-version") {
            fd = open(path.c_str(), O_RDWR); const std::uint32_t obsolete = 1;
            require(fd >= 0 && pwrite(fd, &obsolete, sizeof(obsolete), sizeof(std::uint64_t)) == sizeof(obsolete),
                    "obsolete protocol fixture"); close(fd);
        } else {
            // Even a byte-perfect header copy is not the inode recorded by its writer.
            const auto copy = dir.file("replacement-copy-tmp");
            const int source = open(path.c_str(), O_RDONLY), destination = open(copy.c_str(), O_CREAT | O_EXCL | O_RDWR, 0600);
            require(source >= 0 && destination >= 0, "copy fixture handles");
            std::array<char,4096> bytes{}; ssize_t count;
            while ((count = read(source, bytes.data(), bytes.size())) > 0)
                require(write(destination, bytes.data(), static_cast<std::size_t>(count)) == count, "copy fixture bytes");
            require(count == 0, "copy fixture completed"); close(source); close(destination);
            require(rename(copy.c_str(), path.c_str()) == 0, "copy substitutes wrong mapping inode");
        }
        require(lstat(path.c_str(), &original) == 0, "unsafe fixture original inode");
        { Writer refused(path, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
          require(!refused.valid(), "unsafe predecessor replacement refused"); }
        require(lstat(path.c_str(), &after) == 0 && after.st_ino == original.st_ino && after.st_size == original.st_size,
                "unsafe predecessor path not replaced");
        if (hold_live) {
            Status status;
            require(live->heartbeat() && old.poll_status(monotonic_ns(), status) == ReadResult::ok && old.generation() == generation,
                    "different lock inode cannot retire a live writer");
        }
    }
    const auto symlink = dir.file("replacement-symlink");
    require(::symlink(fresh.c_str(), symlink.c_str()) == 0, "replacement symlink fixture");
    { Writer refused(symlink, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
      require(!refused.valid(), "replacement rejects symlink"); }
    fd = open((fresh + ".lock").c_str(), O_RDWR);
    require(fd >= 0 && flock(fd, LOCK_EX | LOCK_NB) == 0, "external singleton owner fixture");
    { Writer refused(fresh, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
      require(!refused.valid(), "replacement respects unrelated current singleton owner"); }
    close(fd);
}

#ifdef AVSYNC_IPC_TEST_HOOKS
void replacement_busy_reader(TempDir& dir) {
    const auto config = small(); const auto path = dir.file("replacement-reader-busy");
    { Writer writer(path, config); require(writer.valid(), "busy retirement mapping"); }
    Reader reader(path); const auto generation = reader.generation();
    int notice[2], release[2]; require(pipe(notice) == 0 && pipe(release) == 0, "busy retirement pipes");
    const auto child = fork(); require(child >= 0, "busy retirement helper fork");
    if (!child) {
        alarm(5); close(notice[0]); close(release[1]);
        testing::briefly_hold_mutex(path.c_str(), notice[1], release[0]);
    }
    close(notice[1]); close(release[0]); char marker{};
    const bool notified = read(notice[0], &marker, 1) == 1; close(notice[0]);
    bool refused = false;
    {
        Writer attempt(path, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
        refused = !attempt.valid() && attempt.error() == "prior IPC mapping mutex unavailable";
    }
    const bool released = write(release[1], &marker, 1) == 1; close(release[1]);
    int code{}; const auto waited = waitpid(child, &code, 0);
    require(notified && released && waited == child && WIFEXITED(code) && WEXITSTATUS(code) == 0,
            "busy retirement helper bounded and released");
    require(refused && reader.reconnect() && reader.generation() == generation,
            "busy old mutex refuses without replacing its mapping");
    Writer retry(path, config, AllocationPolicy::sparse, ReplacementPolicy::retire_previous);
    require(retry.valid() && retry.generation() != generation, "retirement succeeds after reader releases mutex");
}

void video_publication_retry(TempDir& dir) {
    using P=avsync::VideoPublicationResult;
    const auto c=small(); const auto path=dir.file("video-retry");
    Writer writer(path,c); Reader reader(path);
    require(writer.valid() && reader.valid(),"video retry IPC fixture");
    avsync::Nv12VideoHandoff handoff(c.width,c.height,c.width);
    std::vector<std::byte> source(video_bytes(c),std::byte{0x19});
    const auto captured=monotonic_ns(); constexpr std::int64_t delay=100'000'000;
    require(handoff.try_copy(source,captured)==avsync::VideoHandoffStatus::ok,"first retry frame owned");
    const auto* first=handoff.peek();
    const auto publish=[&](std::span<const std::uint8_t> pixels,std::int64_t capture,std::int64_t presentation) {
        const auto result=writer.try_publish_video(pixels,capture,presentation);
        if (result==WriteResult::ok) return P::published;
        return result==WriteResult::busy ? P::busy : P::failed;
    };
    int notice[2],release[2]; require(pipe(notice)==0 && pipe(release)==0,"video retry pipes");
    const auto child=fork(); require(child>=0,"video retry fork");
    if (!child) {
        close(notice[0]); close(release[1]);
        testing::briefly_hold_mutex(path.c_str(),notice[1],release[0]);
    }
    close(notice[1]); close(release[0]); char marker{};
    const bool notified=read(notice[0],&marker,1)==1; close(notice[0]);
    const auto first_attempt=avsync::try_publish_oldest_video(handoff,captured,delay,publish);
    std::fill(source.begin(),source.end(),std::byte{0x29});
    const auto second_copy=handoff.try_copy(source,captured+16'666'667);
    const auto second_attempt=avsync::try_publish_oldest_video(handoff,captured+1'000'000,delay,publish);
    const bool still_owned=handoff.peek()==first && first->capture_ns==captured && first->pixels.front()==0x19;
    const auto full=handoff.try_copy(source,captured+33'333'334);
    // Release/reap the helper before asserting, including on regression failure.
    const bool released=write(release[1],&marker,1)==1; close(release[1]);
    int status{}; const auto reaped=waitpid(child,&status,0);
    require(notified && released && reaped==child && WIFEXITED(status) && WEXITSTATUS(status)==0,"video retry lock helper");
    require(first_attempt==P::busy && second_attempt==P::busy && still_owned,"busy retains exact video frame");
    require(second_copy==avsync::VideoHandoffStatus::ok && full==avsync::VideoHandoffStatus::full,"bounded retry capture queue");
    std::fill(source.begin(),source.end(),std::byte{0x39});
    require(avsync::try_publish_oldest_video(handoff,captured+20'000'000,delay,publish)==P::published,"retry publishes first frame");
    require(avsync::try_publish_oldest_video(handoff,captured+21'000'000,delay,publish)==P::published,"retry preserves second frame");
    require(!handoff.peek(),"retry drains owned queue");
    std::vector<std::uint8_t> output(video_bytes(c)); FrameInfo info;
    require(reader.read_latest_due_video(captured+delay,output,info)==ReadResult::ok &&
        info.sequence==1 && info.capture_ns==captured && info.presentation_ns==captured+delay &&
        std::all_of(output.begin(),output.end(),[](auto value){ return value==0x19; }),"first retried payload and timestamps exact");
    require(reader.read_latest_due_video(captured+delay+16'666'667,output,info)==ReadResult::ok &&
        info.sequence==2 && info.capture_ns==captured+16'666'667 && info.presentation_ns==captured+delay+16'666'667 &&
        std::all_of(output.begin(),output.end(),[](auto value){ return value==0x29; }),"second retried payload and timestamps exact");
}

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
    std::vector<float> busy_pcm(audio_samples(c,0),0);
    const auto busy_audio = writer.try_publish_audio(0,busy_pcm,1,2);
    const auto busy_heartbeat = writer.try_heartbeat();
    require(write(release[1], &marker, 1) == 1, "release fault owner"); close(release[1]);
    int code{}; require(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 99, "owner exits locked");
    require(busy_result == ReadResult::busy, "reader must not wait for mutex");
    require(busy_publish == WriteResult::busy && busy_audio==WriteResult::busy && busy_heartbeat == WriteResult::busy,
            "capture worker must not wait for an IPC reader mutex");
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected && status.owner_died, "robust recovery rejects incomplete epoch");
    require(!writer.heartbeat(), "dead epoch cannot resume");
    require(writer.try_publish_video(video, 1, 2) == WriteResult::disconnected &&
            writer.try_publish_audio(0,busy_pcm,1,2)==WriteResult::disconnected &&
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
        explicit_process_replacement(dir); replacement_security(dir);
#ifdef AVSYNC_IPC_TEST_HOOKS
        replacement_busy_reader(dir);
        video_publication_retry(dir);
        owner_death_and_wrap(dir);
#else
        std::cout << "SKIP robust owner-death/wrap injection: AVSYNC_IPC_TEST_HOOKS not compiled\n";
#endif
        std::cout << "PASS: IPC scheduling, payload, bounds, stale/drop, restart, security tests. Synthetic only.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
