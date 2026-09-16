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
constexpr std::uint32_t version = 2;
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
    std::uint64_t mapping_device, mapping_inode, singleton_device, singleton_inode;
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

bool same_file(const struct stat& first, const struct stat& second) noexcept {
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino;
}

bool valid_header(const Header& h, std::size_t size, Layout& layout) noexcept {
    if (h.magic_value != magic || h.protocol_version != version || h.header_size != sizeof(Header) ||
        h.mapping_size != size || !h.generation || h.epoch_ns < 0 || !h.mapping_inode || !h.singleton_inode ||
        !layout_for(h.config, layout) || layout.size != size)
        return false;
    for (unsigned s = 0; s < 3; ++s)
        if (h.offsets[s] != layout.offsets[s] || h.strides[s] != layout.strides[s]) return false;
    return true;
}

bool bound_mapping(const Header& h, const struct stat& mapping) noexcept {
    return h.mapping_device == static_cast<std::uint64_t>(mapping.st_dev) &&
           h.mapping_inode == static_cast<std::uint64_t>(mapping.st_ino);
}

bool bound_singleton(const Header& h, const struct stat& singleton) noexcept {
    return h.singleton_device == static_cast<std::uint64_t>(singleton.st_dev) &&
           h.singleton_inode == static_cast<std::uint64_t>(singleton.st_ino);
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

Writer::Writer(std::string path, const Config& c, AllocationPolicy allocation, ReplacementPolicy replacement)
    : impl_(std::make_unique<Impl>()) {
    auto& p = *impl_; p.config = c;
    if (allocation != AllocationPolicy::sparse && allocation != AllocationPolicy::reserve_and_prefault) {
        p.error = "invalid IPC allocation policy"; return;
    }
    if (replacement != ReplacementPolicy::atomic_replace && replacement != ReplacementPolicy::retire_previous) {
        p.error = "invalid IPC replacement policy"; return;
    }
    const bool retire_previous = replacement == ReplacementPolicy::retire_previous;
    if (!validate_config(c, p.error) || !layout_for(c, p.layout)) return;
    std::string parent, leaf;
    if (!split_path(path, parent, leaf)) { p.error = "IPC path must include a parent directory"; return; }
    int dir = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (dir < 0 || fstat(dir, &st) != 0 || st.st_uid != geteuid() || (st.st_mode & 0022)) {
        if (dir >= 0) close(dir);
        p.error = "IPC parent must be user owned and not group/world writable"; return;
    }
    struct stat previous_stat{};
    const bool previous_exists = fstatat(dir, leaf.c_str(), &previous_stat, AT_SYMLINK_NOFOLLOW) == 0;
    if (!previous_exists && errno != ENOENT) {
        close(dir); p.error = "cannot inspect existing IPC path"; return;
    }
    if (previous_exists && (!private_regular(previous_stat) || (retire_previous && previous_stat.st_nlink != 1))) {
        close(dir); p.error = "existing IPC path is not a private regular file"; return;
    }
    const auto lock_name = leaf + ".lock";
    // Existing mappings may only be retired through their EXISTING sidecar.
    // Creating a replacement lock would not exclude a writer holding the old inode.
    const auto create_lock = retire_previous && previous_exists ? 0 : O_CREAT;
    p.singleton_fd = openat(dir, lock_name.c_str(), O_RDWR | create_lock | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    if (p.singleton_fd < 0 || fstat(p.singleton_fd, &st) != 0 || !private_regular(st) ||
        (retire_previous && st.st_nlink != 1) ||
        flock(p.singleton_fd, LOCK_EX | LOCK_NB) != 0) {
        close(dir); p.error = "IPC producer already active or lock file is unsafe"; return;
    }
    const auto singleton_stat = st;
    Map previous;
    Layout previous_layout;
    if (retire_previous && previous_exists) {
        previous.fd = openat(dir, leaf.c_str(), O_RDWR | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
        struct stat opened{};
        if (previous.fd < 0 || fstat(previous.fd, &opened) != 0 || !private_regular(opened) || opened.st_nlink != 1 ||
            !same_file(opened, previous_stat) || opened.st_size < static_cast<off_t>(sizeof(Header)) ||
            opened.st_size > static_cast<off_t>(max_mapping)) {
            close(dir); p.error = "prior IPC mapping identity or size is unsafe"; return;
        }
        previous.size = static_cast<std::size_t>(opened.st_size);
        previous.address = mmap(nullptr, previous.size, PROT_READ | PROT_WRITE, MAP_SHARED, previous.fd, 0);
        if (previous.address == MAP_FAILED || !valid_header(*previous.header(), previous.size, previous_layout) ||
            !bound_mapping(*previous.header(), opened) || !bound_singleton(*previous.header(), singleton_stat)) {
            close(dir); p.error = "prior IPC mapping protocol or inode binding is invalid"; return;
        }
    }
    std::uint64_t generation{};
    if (getrandom(&generation, sizeof(generation), 0) != sizeof(generation))
        generation = static_cast<std::uint64_t>(monotonic_ns()) ^ (std::uint64_t(getpid()) << 32);
    if (!generation) generation = 1;
    const auto temporary = leaf + ".tmp." + std::to_string(getpid()) + "." + std::to_string(generation);
    p.map.fd = openat(dir, temporary.c_str(), O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
    auto fail = [&](const char* message) {
        p.error = message;
        // Never unlink a pre-existing colliding name if O_EXCL did not create it.
        if (p.map.fd >= 0) unlinkat(dir, temporary.c_str(), 0);
        close(dir);
    };
    if (p.map.fd < 0) { fail("cannot create private IPC mapping"); return; }
    if (allocation == AllocationPolicy::reserve_and_prefault) {
        // The temporary file is new and empty. posix_fallocate also extends it;
        // reserving here avoids SIGBUS on the first header write to a full tmpfs.
        // Its return value is an error number, not a failure reported via errno.
        if (posix_fallocate(p.map.fd, 0, static_cast<off_t>(p.layout.size)) != 0) {
            fail("cannot reserve full IPC mapping"); return;
        }
    } else if (ftruncate(p.map.fd, static_cast<off_t>(p.layout.size)) != 0) {
        fail("cannot size IPC mapping"); return;
    }
    p.map.size = p.layout.size;
    p.map.address = mmap(nullptr, p.map.size, PROT_READ | PROT_WRITE, MAP_SHARED, p.map.fd, 0);
    if (p.map.address == MAP_FAILED) { fail("cannot mmap IPC file"); return; }
    if (allocation == AllocationPolicy::reserve_and_prefault) {
        const auto page_size = sysconf(_SC_PAGESIZE);
        if (page_size <= 0 || static_cast<std::uint64_t>(page_size) > max_mapping) {
            fail("cannot determine IPC prefault page size"); return;
        }
        // Only this unpublished, freshly allocated zero-filled mapping is touched.
        // Do not apply this write-prefault operation to readers or existing media.
        auto* pages = static_cast<volatile std::uint8_t*>(p.map.address);
        const auto stride = static_cast<std::size_t>(page_size);
        for (std::size_t offset = 0; offset < p.map.size; offset += stride) pages[offset] = 0;
    }
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
    if (fstat(p.map.fd, &st) != 0) { fail("cannot identify new IPC mapping"); return; }
    h->mapping_device = static_cast<std::uint64_t>(st.st_dev);
    h->mapping_inode = static_cast<std::uint64_t>(st.st_ino);
    h->singleton_device = static_cast<std::uint64_t>(singleton_stat.st_dev);
    h->singleton_inode = static_cast<std::uint64_t>(singleton_stat.st_ino);
    if (retire_previous) {
        // Revalidate names after all allocations, before touching predecessor state.
        struct stat current_lock{}, current_mapping{};
        if (fstatat(dir, lock_name.c_str(), &current_lock, AT_SYMLINK_NOFOLLOW) != 0 ||
            !private_regular(current_lock) || current_lock.st_nlink != 1 || !same_file(current_lock, singleton_stat)) {
            fail("singleton identity changed before IPC publication"); return;
        }
        const bool exists_now = fstatat(dir, leaf.c_str(), &current_mapping, AT_SYMLINK_NOFOLLOW) == 0;
        if (previous_exists != exists_now || (!exists_now && errno != ENOENT) ||
            (exists_now && (!private_regular(current_mapping) || current_mapping.st_nlink != 1 ||
                !same_file(current_mapping, previous_stat) || current_mapping.st_size != previous_stat.st_size))) {
            fail("mapping identity changed before IPC publication"); return;
        }
        if (previous_exists) {
            // Do not use a blocking lock: an active reader can hold the mutex.
            // No mutation occurs for EBUSY or unrecognized robust-mutex state.
            auto* prior = previous.header();
            const auto locked = pthread_mutex_trylock(&prior->mutex);
            if (locked != 0 && locked != EOWNERDEAD) {
                fail("prior IPC mapping mutex unavailable"); return;
            }
            prior->online = 0;
            if (locked == EOWNERDEAD) {
                prior->owner_died = 1;
                if (pthread_mutex_consistent(&prior->mutex) != 0) {
                    pthread_mutex_unlock(&prior->mutex);
                    fail("cannot retire abandoned IPC mapping mutex"); return;
                }
            }
            // Retirement and publication are ordered while the old mutex is held.
            // Existing readers can never consume old queued PCM after successor visibility.
            const auto published = renameat(dir, temporary.c_str(), dir, leaf.c_str());
            pthread_mutex_unlock(&prior->mutex);
            if (published != 0) { fail("cannot publish IPC mapping after retirement"); return; }
            close(dir); p.ready = true; return;
        }
    }
    if (renameat(dir, temporary.c_str(), dir, leaf.c_str()) != 0) { fail("cannot publish IPC mapping"); return; }
    close(dir); p.ready = true;
}
Writer::~Writer() = default;
bool Writer::valid() const noexcept { return impl_->ready; }
const std::string& Writer::error() const noexcept { return impl_->error; }
std::uint64_t Writer::generation() const noexcept { return valid() ? impl_->map.header()->generation : 0; }
std::int64_t Writer::epoch_ns() const noexcept { return valid() ? impl_->map.header()->epoch_ns : 0; }

namespace {
WriteResult publish(Map& map, const Layout& layout, unsigned stream, const void* bytes, std::size_t count,
             std::uint32_t frames, std::int64_t capture, std::int64_t presentation,
             bool nonblocking = false) noexcept {
    if (capture < 0 || presentation < capture) return WriteResult::invalid;
    auto* h = map.header(); Lock lock(h, nonblocking);
    if (lock.result == EBUSY) return WriteResult::busy;
    if (!lock.held) return WriteResult::invalid;
    if (lock.result != 0 || !h->online || h->owner_died) return WriteResult::disconnected;
    if (h->sequences[stream] == std::numeric_limits<std::uint64_t>::max()) return WriteResult::invalid;
    if (h->sequences[stream]) {
        const auto* prior = map.slot(layout, stream, h->sequences[stream]);
        if (capture < prior->info.capture_ns || presentation < prior->info.presentation_ns) return WriteResult::invalid;
    }
    const auto sequence = h->sequences[stream] + 1;
    auto* slot = map.slot(layout, stream, sequence);
    slot->info.sequence = 0;
    std::memcpy(reinterpret_cast<std::uint8_t*>(slot) + sizeof(Slot), bytes, count);
    slot->info = {h->generation, sequence, capture, presentation, frames, static_cast<std::uint32_t>(count)};
    h->sequences[stream] = sequence;
    h->heartbeat_ns = monotonic_ns();
    return WriteResult::ok;
}
} // namespace

bool Writer::publish_video(std::span<const std::uint8_t> pixels, std::int64_t capture,
                           std::int64_t presentation) noexcept {
    if (!valid() || pixels.size() != video_bytes(impl_->config)) return false;
    return publish(impl_->map, impl_->layout, 0, pixels.data(), pixels.size(), 1, capture, presentation) == WriteResult::ok;
}
WriteResult Writer::try_publish_video(std::span<const std::uint8_t> pixels, std::int64_t capture,
                                     std::int64_t presentation) noexcept {
    if (!valid() || pixels.size() != video_bytes(impl_->config)) return WriteResult::invalid;
    return publish(impl_->map, impl_->layout, 0, pixels.data(), pixels.size(), 1, capture, presentation, true);
}
WriteResult Writer::try_heartbeat() noexcept {
    if (!valid()) return WriteResult::invalid;
    Lock lock(impl_->map.header(), true);
    if (lock.result == EBUSY) return WriteResult::busy;
    if (!lock.held) return WriteResult::invalid;
    if (lock.result != 0 || !lock.h->online || lock.h->owner_died) return WriteResult::disconnected;
    lock.h->heartbeat_ns = monotonic_ns();
    return WriteResult::ok;
}
bool Writer::publish_audio(unsigned stream, std::span<const float> samples, std::int64_t capture,
                           std::int64_t presentation) noexcept {
    if (!valid() || stream > 1 || samples.size() != audio_samples(impl_->config, stream)) return false;
    return publish(impl_->map, impl_->layout, stream + 1, samples.data(), samples.size_bytes(),
                   impl_->config.audio_frames, capture, presentation) == WriteResult::ok;
}
bool Writer::heartbeat() noexcept {
    if (!valid()) return false;
    Lock lock(impl_->map.header(), false);
    if (lock.result != 0 || !lock.h->online) return false;
    lock.h->heartbeat_ns = monotonic_ns(); return true;
}
WriteResult Writer::try_publish_audio(unsigned stream, std::span<const float> samples,
        std::int64_t capture, std::int64_t presentation) noexcept {
    if (!valid() || stream>1 || samples.size()!=audio_samples(impl_->config,stream)) return WriteResult::invalid;
    return publish(impl_->map,impl_->layout,stream+1,samples.data(),samples.size_bytes(),
                   impl_->config.audio_frames,capture,presentation,true);
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
    if (!valid_header(*h, p.map.size, p.layout) || !bound_mapping(*h, st)) {
        p.error = "IPC protocol/header/layout or inode binding mismatch"; return false;
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
    return read_next_audio(stream, now, 0, dst, info, max_age);
}

ReadResult Reader::read_next_audio(unsigned stream, std::int64_t now, std::int64_t max_future,
                                 std::span<float> dst, FrameInfo& info, std::int64_t max_age) noexcept {
    auto& p = *impl_;
    if (!valid() || stream > 1 || now < 0 || max_age < 0 ||
        max_future < 0 || max_future > max_audio_lookahead_ns) return ReadResult::invalid;
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
        // Both timestamps are nonnegative. Subtract only the smaller from the
        // larger, avoiding overflow from constructing a now + future deadline.
        const auto presentation = slot->info.presentation_ns;
        if (presentation > now && presentation - now > max_future) break;
        p.cursors[s] = sequence;
        if (presentation <= now && now - presentation > max_age) { ++p.skipped[s]; stale = true; continue; }
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
[[noreturn]] void hold_test_mutex(const char* path, int notify_fd, int release_fd, bool abandon) {
    const int fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
    struct stat st{};
    if (fd < 0 || fstat(fd, &st) || !private_regular(st)) _exit(91);
    auto* h = static_cast<Header*>(mmap(nullptr, sizeof(Header), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    if (h == MAP_FAILED || h->magic_value != magic || pthread_mutex_lock(&h->mutex)) _exit(92);
    const char ready = 'R';
    if (write(notify_fd, &ready, 1) != 1) _exit(93);
    char release{};
    if (read(release_fd, &release, 1) != 1) _exit(94);
    if (!abandon) { pthread_mutex_unlock(&h->mutex); _exit(0); }
    _exit(99); // Kernel marks the robust mutex owner dead; no unlock.
}
[[noreturn]] void abandon_mutex(const char* path,int notify_fd,int release_fd) {
    hold_test_mutex(path,notify_fd,release_fd,true);
}
[[noreturn]] void briefly_hold_mutex(const char* path,int notify_fd,int release_fd) {
    hold_test_mutex(path,notify_fd,release_fd,false);
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
