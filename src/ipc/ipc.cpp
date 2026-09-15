// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/ipc.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <sys/file.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace avsync::ipc {
namespace {
constexpr std::uint64_t magic = 0x415653594E433031ULL;
constexpr std::uint32_t version = 1;
constexpr std::size_t max_mapping = 2ULL * 1024 * 1024 * 1024;
constexpr std::int64_t heartbeat_timeout = 2000000000LL;
constexpr std::size_t align64(std::size_t n) { return (n + 63U) & ~std::size_t(63U); }

struct Header {
    std::uint64_t magic_value;
    std::uint32_t protocol_version, header_size;
    std::uint64_t mapping_size, generation;
    Config config;
    std::int64_t epoch_ns, heartbeat_ns;
    std::uint64_t offsets[3], strides[3], sequences[3];
    std::uint32_t online, owner_died;
    pthread_mutex_t mutex;
};
struct Slot { FrameInfo info; };
struct Layout {
    std::size_t size{}, offsets[3]{}, strides[3]{};
    std::uint32_t capacities[3]{};
};

bool layout_for(const Config& c, Layout& l) {
    if (c.width < 2 || c.height < 2 || c.width > 3840 || c.height > 2160 ||
        (c.width & 1U) || (c.height & 1U) || c.fps == 0 || c.fps > 120 ||
        c.video_capacity < 2 || c.video_capacity > 2400 ||
        c.audio_capacity < 2 || c.audio_capacity > 12000 ||
        c.audio_rate != 48000 || c.audio_frames == 0 || c.audio_frames > 4800) return false;
    l.capacities[0] = c.video_capacity;
    l.capacities[1] = l.capacities[2] = c.audio_capacity;
    l.strides[0] = align64(sizeof(Slot) + video_bytes(c));
    l.strides[1] = align64(sizeof(Slot) + audio_samples(c, 0) * sizeof(float));
    l.strides[2] = align64(sizeof(Slot) + audio_samples(c, 1) * sizeof(float));
    l.size = align64(sizeof(Header));
    for (unsigned s = 0; s != 3; ++s) {
        l.offsets[s] = l.size;
        if (l.strides[s] > max_mapping / l.capacities[s]) return false;
        const auto bytes = l.strides[s] * l.capacities[s];
        if (bytes > max_mapping || l.size > max_mapping - bytes) return false;
        l.size += bytes;
    }
    return true;
}

bool private_regular(const struct stat& st) {
    return S_ISREG(st.st_mode) && st.st_uid == geteuid() && (st.st_mode & 0777) == 0600;
}

bool split_path(const std::string& path, std::string& dir, std::string& leaf) {
    const auto at = path.find_last_of('/');
    if (at == std::string::npos || at + 1 == path.size()) return false;
    dir = at == 0 ? "/" : path.substr(0, at);
    leaf = path.substr(at + 1);
    return leaf != "." && leaf != "..";
}

struct Map {
    int fd{-1};
    void* address{MAP_FAILED};
    std::size_t size{};
    ~Map() { clear(); }
    void clear() noexcept {
        if (address != MAP_FAILED) munmap(address, size);
        if (fd >= 0) close(fd);
        address = MAP_FAILED; fd = -1; size = 0;
    }
    Header* header() const noexcept { return static_cast<Header*>(address); }
    Slot* slot(const Layout& l, unsigned stream, std::uint64_t sequence) const noexcept {
        const auto index = (sequence - 1) % l.capacities[stream];
        return reinterpret_cast<Slot*>(static_cast<std::uint8_t*>(address) +
                                       l.offsets[stream] + index * l.strides[stream]);
    }
};

struct Lock {
    Header* h;
    bool held{};
    int result;
    Lock(Header* header, bool nonblocking) noexcept : h(header) {
        result = nonblocking ? pthread_mutex_trylock(&h->mutex) : pthread_mutex_lock(&h->mutex);
        held = result == 0 || result == EOWNERDEAD;
        if (result == EOWNERDEAD) {
            // A producer may have died half-way through a slot copy. Reject this epoch.
            h->online = 0; h->owner_died = 1;
            pthread_mutex_consistent(&h->mutex);
        }
    }
    ~Lock() { if (held) pthread_mutex_unlock(&h->mutex); }
};

ReadResult check_locked(const Lock& lock, std::int64_t now) noexcept {
    if (lock.result == EBUSY) return ReadResult::busy;
    if (!lock.held) return ReadResult::invalid;
    if (lock.result == EOWNERDEAD || lock.h->owner_died || !lock.h->online)
        return ReadResult::disconnected;
    if (now < 0 || lock.h->heartbeat_ns < 0) return ReadResult::invalid;
    if (now >= lock.h->heartbeat_ns && now - lock.h->heartbeat_ns > heartbeat_timeout)
        return ReadResult::disconnected;
    return ReadResult::ok;
}

bool valid_info(const FrameInfo& i, std::uint64_t generation, std::uint64_t sequence,
                std::uint32_t bytes, std::uint32_t frames) noexcept {
    return i.generation == generation && i.sequence == sequence && i.bytes == bytes &&
           i.frames == frames && i.capture_ns >= 0 && i.presentation_ns >= i.capture_ns;
}
} // namespace

std::int64_t monotonic_ns() noexcept {
    timespec t{};
    if (clock_gettime(CLOCK_MONOTONIC, &t) != 0) return 0;
    return static_cast<std::int64_t>(t.tv_sec) * 1000000000LL + t.tv_nsec;
}
std::size_t video_bytes(const Config& c) noexcept {
    return static_cast<std::size_t>(c.width) * c.height * 3 / 2;
}
std::size_t audio_samples(const Config& c, unsigned stream) noexcept {
    return stream < 2 ? static_cast<std::size_t>(c.audio_frames) * (stream == 0 ? 2 : 1) : 0;
}
bool validate_config(const Config& config, std::string& error) noexcept {
    Layout l;
    if (!layout_for(config, l)) {
        error = "invalid IPC dimensions/rate/capacity or mapping exceeds 2 GiB";
        return false;
    }
    error.clear(); return true;
}
const char* result_name(ReadResult r) noexcept {
    switch (r) {
    case ReadResult::ok: return "ok";
    case ReadResult::empty: return "empty";
    case ReadResult::busy: return "busy";
    case ReadResult::disconnected: return "disconnected";
    case ReadResult::invalid: return "invalid";
    case ReadResult::buffer_too_small: return "buffer_too_small";
    case ReadResult::stale: return "stale";
    }
    return "unknown";
}

struct Writer::Impl {
    Map map;
    Layout layout;
    Config config;
    std::string error;
    int singleton_fd{-1};
    bool ready{};
    ~Impl() {
        if (ready) {
            Lock lock(map.header(), false);
            if (lock.held) { map.header()->online = 0; map.header()->heartbeat_ns = monotonic_ns(); }
        }
        if (singleton_fd >= 0) close(singleton_fd);
    }
};

Writer::Writer(std::string path, const Config& c) : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_; p.config = c;
    if (!validate_config(c, p.error) || !layout_for(c, p.layout)) return;
    std::string parent, leaf;
    if (!split_path(path, parent, leaf)) { p.error = "IPC path must include a parent directory"; return; }
    int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (dir < 0 || fstat(dir, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        if (dir >= 0) close(dir);
        p.error = "IPC parent must be user owned and not group/world writable"; return;
    }
    if (fstatat(dir, leaf.c_str(), &st, AT_SYMLINK_NOFOLLOW) == 0 && !private_regular(st)) {
        close(dir); p.error = "existing IPC path is not a private regular file"; return;
    }
    const auto lock_name = leaf + ".lock";
    p.singleton_fd = openat(dir, lock_name.c_str(), O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (p.singleton_fd < 0 || fstat(p.singleton_fd, &st) != 0 || !private_regular(st) ||
        flock(p.singleton_fd, LOCK_EX | LOCK_NB) != 0) {
        close(dir); p.error = "IPC producer already active or lock file is unsafe"; return;
    }
    std::uint64_t generation{};
    if (getrandom(&generation, sizeof(generation), 0) != sizeof(generation))
        generation = static_cast<std::uint64_t>(monotonic_ns()) ^ (std::uint64_t(getpid()) << 32);
    if (!generation) generation = 1;
    const auto temporary = leaf + ".tmp." + std::to_string(getpid()) + "." + std::to_string(generation);
    p.map.fd = openat(dir, temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    auto fail = [&](const char* message) { p.error = message; unlinkat(dir, temporary.c_str(), 0); close(dir); };
    if (p.map.fd < 0) { fail("cannot create private IPC mapping"); return; }
    if (ftruncate(p.map.fd, static_cast<off_t>(p.layout.size)) != 0) { fail("cannot size IPC mapping"); return; }
    p.map.size = p.layout.size;
    p.map.address = mmap(nullptr, p.map.size, PROT_READ | PROT_WRITE, MAP_SHARED, p.map.fd, 0);
    if (p.map.address == MAP_FAILED) { fail("cannot mmap IPC file"); return; }
    auto* h = p.map.header();
    std::construct_at(h, Header{});
    pthread_mutexattr_t attr;
    if (pthread_mutexattr_init(&attr) != 0) { fail("cannot initialize mutex attributes"); return; }
    const bool mutex_ok = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED) == 0 &&
                          pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST) == 0 &&
                          pthread_mutex_init(&h->mutex, &attr) == 0;
    pthread_mutexattr_destroy(&attr);
    if (!mutex_ok) { fail("cannot initialize robust process-shared mutex"); return; }
    h->protocol_version = version; h->header_size = sizeof(Header); h->mapping_size = p.layout.size;
    h->generation = generation; h->config = c;
    h->epoch_ns = h->heartbeat_ns = monotonic_ns(); h->online = 1;
    for (unsigned s = 0; s < 3; ++s) { h->offsets[s] = p.layout.offsets[s]; h->strides[s] = p.layout.strides[s]; }
    h->magic_value = magic;
    if (renameat(dir, temporary.c_str(), dir, leaf.c_str()) != 0) { fail("cannot publish IPC mapping"); return; }
    close(dir); p.ready = true;
}
Writer::~Writer() = default;
bool Writer::valid() const noexcept { return impl_->ready; }
const std::string& Writer::error() const noexcept { return impl_->error; }
std::uint64_t Writer::generation() const noexcept { return valid() ? impl_->map.header()->generation : 0; }
std::int64_t Writer::epoch_ns() const noexcept { return valid() ? impl_->map.header()->epoch_ns : 0; }

namespace {
bool publish(Map& map, const Layout& layout, unsigned stream, const void* bytes, std::size_t count,
             std::uint32_t frames, std::int64_t capture, std::int64_t presentation) noexcept {
    if (capture < 0 || presentation < capture) return false;
    auto* h = map.header(); Lock lock(h, false);
    if (lock.result != 0 || !h->online || h->owner_died) return false;
    if (h->sequences[stream] == std::numeric_limits<std::uint64_t>::max()) return false;
    if (h->sequences[stream]) {
        const auto* prior = map.slot(layout, stream, h->sequences[stream]);
        if (capture < prior->info.capture_ns || presentation < prior->info.presentation_ns) return false;
    }
    const auto sequence = h->sequences[stream] + 1;
    auto* slot = map.slot(layout, stream, sequence);
    slot->info.sequence = 0;
    std::memcpy(reinterpret_cast<std::uint8_t*>(slot) + sizeof(Slot), bytes, count);
    slot->info = {h->generation, sequence, capture, presentation, frames, static_cast<std::uint32_t>(count)};
    h->sequences[stream] = sequence;
    h->heartbeat_ns = monotonic_ns();
    return true;
}
} // namespace

bool Writer::publish_video(std::span<const std::uint8_t> pixels, std::int64_t capture,
                           std::int64_t presentation) noexcept {
    if (!valid() || pixels.size() != video_bytes(impl_->config)) return false;
    return publish(impl_->map, impl_->layout, 0, pixels.data(), pixels.size(), 1, capture, presentation);
}
bool Writer::publish_audio(unsigned stream, std::span<const float> samples, std::int64_t capture,
                           std::int64_t presentation) noexcept {
    if (!valid() || stream > 1 || samples.size() != audio_samples(impl_->config, stream)) return false;
    return publish(impl_->map, impl_->layout, stream + 1, samples.data(), samples.size_bytes(),
                   impl_->config.audio_frames, capture, presentation);
}
bool Writer::heartbeat() noexcept {
    if (!valid()) return false;
    Lock lock(impl_->map.header(), false);
    if (lock.result != 0 || !lock.h->online) return false;
    lock.h->heartbeat_ns = monotonic_ns(); return true;
}

struct Reader::Impl {
    std::string path, error;
    Map map;
    Layout layout;
    Config config{};
    std::uint64_t generation{}, cursors[3]{}, skipped[3]{};
    bool ready{};
};

Reader::Reader(std::string path) : impl_(std::make_unique<Impl>()) {
    impl_->path = std::move(path); reconnect();
}
Reader::~Reader() = default;
bool Reader::valid() const noexcept { return impl_->ready; }
const std::string& Reader::error() const noexcept { return impl_->error; }
const Config& Reader::config() const noexcept { return impl_->config; }
std::uint64_t Reader::generation() const noexcept { return impl_->generation; }
bool Reader::reconnect() {
    auto& p = *impl_; p.ready = false; p.generation = 0; p.map.clear(); p.error.clear();
    std::fill(std::begin(p.cursors), std::end(p.cursors), 0);
    std::fill(std::begin(p.skipped), std::end(p.skipped), 0);
    p.map.fd = open(p.path.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (p.map.fd < 0 || fstat(p.map.fd, &st) != 0 || !private_regular(st) ||
        st.st_size < static_cast<off_t>(sizeof(Header)) || st.st_size > static_cast<off_t>(max_mapping)) {
        p.error = "IPC mapping missing, unsafe or invalid size"; return false;
    }
    p.map.size = static_cast<std::size_t>(st.st_size);
    p.map.address = mmap(nullptr, p.map.size, PROT_READ | PROT_WRITE, MAP_SHARED, p.map.fd, 0);
    if (p.map.address == MAP_FAILED) { p.error = "cannot map IPC reader"; return false; }
    auto* h = p.map.header();
    if (h->magic_value != magic || h->protocol_version != version || h->header_size != sizeof(Header) ||
        h->mapping_size != p.map.size || !h->generation || !layout_for(h->config, p.layout) ||
        p.layout.size != p.map.size) { p.error = "IPC protocol/header/layout mismatch"; return false; }
    for (unsigned s = 0; s < 3; ++s)
        if (h->offsets[s] != p.layout.offsets[s] || h->strides[s] != p.layout.strides[s]) {
            p.error = "IPC region bounds mismatch"; return false;
        }
    p.config = h->config; p.generation = h->generation; p.ready = true; return true;
}

ReadResult Reader::read_latest_due_video(std::int64_t now, std::span<std::uint8_t> dst,
                                       FrameInfo& info, std::int64_t max_age) noexcept {
    auto& p = *impl_;
    if (!valid() || max_age < 0) return ReadResult::invalid;
    const auto count = video_bytes(p.config);
    if (dst.size() < count) return ReadResult::buffer_too_small;
    Lock lock(p.map.header(), true);
    auto result = check_locked(lock, now); if (result != ReadResult::ok) return result;
    const auto newest = lock.h->sequences[0];
    if (newest <= p.cursors[0]) return ReadResult::empty;
    const auto oldest = newest >= p.config.video_capacity ? newest - p.config.video_capacity + 1 : 1;
    for (auto sequence = newest; sequence >= oldest && sequence > p.cursors[0]; --sequence) {
        auto* slot = p.map.slot(p.layout, 0, sequence);
        if (!valid_info(slot->info, p.generation, sequence, static_cast<std::uint32_t>(count), 1))
            return ReadResult::invalid;
        if (slot->info.presentation_ns > now) continue;
        p.skipped[0] += sequence - p.cursors[0] - 1;
        p.cursors[0] = sequence;
        if (now - slot->info.presentation_ns > max_age) { ++p.skipped[0]; return ReadResult::stale; }
        info = slot->info;
        std::memcpy(dst.data(), reinterpret_cast<std::uint8_t*>(slot) + sizeof(Slot), count);
        return ReadResult::ok;
    }
    return ReadResult::empty;
}

ReadResult Reader::read_next_due_audio(unsigned stream, std::int64_t now, std::span<float> dst,
                                     FrameInfo& info, std::int64_t max_age) noexcept {
    auto& p = *impl_;
    if (!valid() || stream > 1 || max_age < 0) return ReadResult::invalid;
    const auto count = audio_samples(p.config, stream) * sizeof(float);
    if (dst.size_bytes() < count) return ReadResult::buffer_too_small;
    Lock lock(p.map.header(), true);
    auto result = check_locked(lock, now); if (result != ReadResult::ok) return result;
    const auto s = stream + 1; const auto newest = lock.h->sequences[s];
    if (newest <= p.cursors[s]) return ReadResult::empty;
    const auto oldest = newest >= p.config.audio_capacity ? newest - p.config.audio_capacity + 1 : 1;
    auto next = p.cursors[s] + 1;
    if (next < oldest) { p.skipped[s] += oldest - next; next = oldest; p.cursors[s] = oldest - 1; }
    bool stale = false;
    const auto available = newest - next + 1; // <= capacity, even at UINT64_MAX.
    for (std::uint64_t offset = 0; offset < available; ++offset) {
        const auto sequence = next + offset;
        auto* slot = p.map.slot(p.layout, s, sequence);
        if (!valid_info(slot->info, p.generation, sequence, static_cast<std::uint32_t>(count), p.config.audio_frames))
            return ReadResult::invalid;
        if (slot->info.presentation_ns > now) break;
        p.cursors[s] = sequence;
        if (now - slot->info.presentation_ns > max_age) { ++p.skipped[s]; stale = true; continue; }
        info = slot->info;
        std::memcpy(dst.data(), reinterpret_cast<std::uint8_t*>(slot) + sizeof(Slot), count);
        return ReadResult::ok;
    }
    return stale ? ReadResult::stale : ReadResult::empty;
}

ReadResult Reader::poll_status(std::int64_t now, Status& status) noexcept {
    if (!valid()) return ReadResult::invalid;
    auto& p = *impl_; Lock lock(p.map.header(), true);
    const auto result = check_locked(lock, now);
    if (!lock.held) return result;
    status = {p.generation, lock.h->epoch_ns, lock.h->heartbeat_ns,
              lock.h->sequences[0], {lock.h->sequences[1], lock.h->sequences[2]},
              p.skipped[0], {p.skipped[1], p.skipped[2]},
              result == ReadResult::ok, lock.h->owner_died != 0};
    return result;
}

#ifdef AVSYNC_IPC_TEST_HOOKS
// Private fault-injection symbols, deliberately absent from the public API/CLI.
// Only compile these in test builds. They operate on the test process's own IPC file.
namespace testing {
[[noreturn]] void abandon_mutex(const char* path, int notify_fd, int release_fd) {
    const int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || !private_regular(st)) _exit(91);
    auto* h = static_cast<Header*>(mmap(nullptr, sizeof(Header), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (h == MAP_FAILED || h->magic_value != magic || pthread_mutex_lock(&h->mutex)) _exit(92);
    const char ready = 'R';
    if (write(notify_fd, &ready, 1) != 1) _exit(93);
    char release{};
    if (read(release_fd, &release, 1) != 1) _exit(94);
    _exit(99); // Kernel marks the robust mutex owner dead; no unlock.
}
bool set_sequence(const char* path, unsigned stream, std::uint64_t value) {
    if (stream > 2) return false;
    const int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || !private_regular(st)) { if (fd >= 0) close(fd); return false; }
    auto* h = static_cast<Header*>(mmap(nullptr, sizeof(Header), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (h == MAP_FAILED) { close(fd); return false; }
    bool ok = false;
    { Lock lock(h, false); if (lock.result == 0) { h->sequences[stream] = value; ok = true; } }
    munmap(h, sizeof(Header)); close(fd); return ok;
}
} // namespace testing
#endif

} // namespace avsync::ipc
