// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"
#include <array>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
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
    require(writer.publish_audio(0, samples, t, t + 100000000), "audio publish");
    require(reader.read_next_due_audio(0, t, pcm, info) == ReadResult::empty, "must not release future audio");
    require(reader.read_next_due_audio(0, t + 100000000, pcm, info) == ReadResult::ok && samples == pcm, "audio payload");
    require(reader.read_next_due_audio(2, t, pcm, info) == ReadResult::invalid, "invalid audio stream");
    require(reader.read_next_due_audio(0, t, pcm, info, -1) == ReadResult::invalid, "negative stale limit");
    for (unsigned i = 1; i <= 10; ++i) {
        require(writer.publish_video(input, t + i * 10000000, t + 100000000 + i * 10000000), "ring video publish");
        require(writer.publish_audio(0, samples, t + i * 10000000, t + 100000000 + i * 10000000), "ring audio publish");
    }
    require(reader.read_latest_due_video(t + 200000000, output, info) == ReadResult::ok && info.sequence == 11, "newest due after overwrite");
    require(reader.read_next_due_audio(0, t + 500000000, pcm, info, 1000000) == ReadResult::stale, "stale audio dropped");
    require(reader.read_next_due_audio(0, t + 500000000, pcm, info) == ReadResult::empty, "no stale catch-up");
    Status status;
    require(reader.poll_status(t + 500000000, status) == ReadResult::ok && status.skipped_video == 9 && status.skipped_audio[0] == 10, "skip counters");
    require(reader.poll_status(t + 3000000000LL, status) == ReadResult::disconnected, "heartbeat expiry");
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
    require(write(release[1], &marker, 1) == 1, "release fault owner"); close(release[1]);
    int code{}; require(waitpid(child, &code, 0) == child && WIFEXITED(code) && WEXITSTATUS(code) == 99, "owner exits locked");
    require(busy_result == ReadResult::busy, "reader must not wait for mutex");
    require(reader.poll_status(monotonic_ns(), status) == ReadResult::disconnected && status.owner_died, "robust recovery rejects incomplete epoch");
    require(!writer.heartbeat(), "dead epoch cannot resume");
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
        TempDir dir; scheduling_and_bounds(dir); restart_and_security(dir);
#ifdef AVSYNC_IPC_TEST_HOOKS
        owner_death_and_wrap(dir);
#else
        std::cout << "SKIP robust owner-death/wrap injection: AVSYNC_IPC_TEST_HOOKS not compiled\n";
#endif
        std::cout << "PASS: IPC scheduling, payload, bounds, stale/drop, restart, security tests. Synthetic only.\n";
        return 0;
    } catch (const std::exception& e) { std::cerr << "FAIL: " << e.what() << '\n'; return 1; }
}
