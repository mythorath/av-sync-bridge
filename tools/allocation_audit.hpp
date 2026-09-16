// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstddef>
#include <cstdint>
// Test-only, glibc-specific interposer. Never load into OBS or startup services.
struct AllocationStats {
    std::uint64_t attempts{}, failures{}, live{}, bytes{}, peak{}, table_overflow{};
};
extern "C" bool avsync_audit_begin(std::uint64_t fail_at) noexcept;
extern "C" void avsync_audit_enable(bool enabled) noexcept;
extern "C" AllocationStats avsync_audit_stats() noexcept;
