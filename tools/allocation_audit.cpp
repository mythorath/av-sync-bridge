// SPDX-License-Identifier: GPL-2.0-or-later
// Test-only Linux/glibc allocator interposition. No logging/heap use in hooks.
// Deliberately uses glibc entrypoints rather than recursively resolving dlsym.
// Counts requested heap bytes, NOT RSS, allocator arena overhead, mmap or stacks.
#include "allocation_audit.hpp"
#include <cerrno>
#include <limits>
#include <features.h>
#if !defined(__GLIBC__)
#error "Allocation audit requires glibc"
#endif
extern "C" void* __libc_malloc(std::size_t) noexcept;
extern "C" void* __libc_calloc(std::size_t,std::size_t) noexcept;
extern "C" void* __libc_realloc(void*,std::size_t) noexcept;
extern "C" void __libc_free(void*) noexcept;
namespace {
struct Entry { void* pointer{}; std::size_t size{}; };
struct Audit {
    bool enabled{};
    std::uint64_t fail_at{};
    AllocationStats stats;
    Entry entries[512]{};
};
// Only the explicitly enabled test thread is counted. Library has no workers.
thread_local Audit audit;
bool fail() noexcept {
    if (!audit.enabled) return false;
    ++audit.stats.attempts;
    if (audit.fail_at && audit.stats.attempts==audit.fail_at) {
        ++audit.stats.failures; errno=ENOMEM; return true;
    }
    return false;
}
void add(void* p,std::size_t n) noexcept {
    if (!audit.enabled || !p) return;
    for (auto& e:audit.entries) if (!e.pointer) {
        e={p,n}; ++audit.stats.live; audit.stats.bytes+=n;
        if (audit.stats.bytes>audit.stats.peak) audit.stats.peak=audit.stats.bytes;
        return;
    }
    ++audit.stats.table_overflow;
}
void remove(void* p) noexcept {
    if (!p) return;
    for (auto& e:audit.entries) if (e.pointer==p) {
        --audit.stats.live; audit.stats.bytes-=e.size; e={}; return;
    }
}
}
extern "C" bool avsync_audit_begin(std::uint64_t fail_at) noexcept {
    if (audit.stats.live) return false;
    audit={}; audit.fail_at=fail_at; audit.enabled=true; return true;
}
extern "C" void avsync_audit_enable(bool enabled) noexcept { audit.enabled=enabled; }
extern "C" AllocationStats avsync_audit_stats() noexcept { return audit.stats; }
extern "C" void* malloc(std::size_t n) noexcept {
    if (fail()) return nullptr;
    auto* p=__libc_malloc(n); add(p,n); return p;
}
extern "C" void* calloc(std::size_t n,std::size_t size) noexcept {
    if (fail()) return nullptr;
    auto* p=__libc_calloc(n,size);
    if (size && n>std::numeric_limits<std::size_t>::max()/size) return p;
    add(p,n*size); return p;
}
extern "C" void* realloc(void* old,std::size_t n) noexcept {
    if (fail()) return nullptr;
    // Capture identity numerically before realloc invalidates the old object.
    const auto address=reinterpret_cast<std::uintptr_t>(old);
    auto* p=__libc_realloc(old,n);
    if (p || n==0) { remove(reinterpret_cast<void*>(address)); add(p,n); }
    return p;
}
extern "C" void free(void* p) noexcept { remove(p); __libc_free(p); }
