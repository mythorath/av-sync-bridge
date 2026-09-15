// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <vector>

namespace avsync {

enum class VideoHandoffStatus { ok, full, invalid };

// Exactly one producer calls try_copy; exactly one consumer calls peek/release.
// Construct before starting either thread and join both before destruction.
// No operation after construction allocates, waits, or performs I/O. Payload
// storage supplied to try_copy must remain valid and not alias this queue.
class Nv12VideoHandoff {
public:
    struct Packet {
        std::vector<std::uint8_t> pixels;
        std::int64_t capture_ns = 0;
    };

    static constexpr std::size_t maximum_payload_bytes = 32U * 1024U * 1024U;

    Nv12VideoHandoff(std::uint32_t width, std::uint32_t height, std::uint32_t stride)
    {
        if (width == 0 || height == 0 || width > 3840 || height > 2160 ||
            width % 2 != 0 || height % 2 != 0 || stride < width || stride % 2 != 0)
            throw std::invalid_argument("unsupported progressive NV12 dimensions or stride");
        // With the checked dimension bounds, both products fit uint64_t even
        // for maximum uint32_t stride. Check before narrowing to size_t.
        const auto rows = static_cast<std::uint64_t>(height) + height / 2;
        const auto required = rows * stride;
        const auto packed = rows * width;
        if (required > maximum_payload_bytes ||
            required > std::numeric_limits<std::size_t>::max())
            throw std::invalid_argument("NV12 source exceeds bounded handoff capacity");
        width_ = width;
        rows_ = static_cast<std::size_t>(rows);
        stride_ = stride;
        required_bytes_ = static_cast<std::size_t>(required);
        packed_bytes_ = static_cast<std::size_t>(packed);
        for (auto& slot : slots_)
            slot.packet.pixels.resize(packed_bytes_);
    }

    Nv12VideoHandoff(const Nv12VideoHandoff&) = delete;
    Nv12VideoHandoff& operator=(const Nv12VideoHandoff&) = delete;

    [[nodiscard]] VideoHandoffStatus try_copy(
        std::span<const std::byte> source, std::int64_t capture_ns) noexcept
    {
        if (capture_ns < 0 || source.data() == nullptr ||
            source.size() < required_bytes_ || source.size() > maximum_payload_bytes)
            return VideoHandoffStatus::invalid;
        auto& slot = slots_[producer_index_];
        // Acquire pairs with consumer release before reusing pixel storage.
        if (slot.ready.load(std::memory_order_acquire))
            return VideoHandoffStatus::full;
        auto* destination = slot.packet.pixels.data();
        if (stride_ == width_) {
            std::memcpy(destination, source.data(), packed_bytes_);
        } else {
            // Linear NV12 has height Y rows followed by height/2 interleaved UV
            // rows with the same source stride. Do not retain padding bytes.
            for (std::size_t row = 0; row < rows_; ++row)
                std::memcpy(destination + row * width_, source.data() + row * stride_, width_);
        }
        slot.packet.capture_ns = capture_ns;
        // Pixels and their original timestamp become visible atomically as one
        // slot publication. The producer cannot reuse it until consumer release.
        slot.ready.store(true, std::memory_order_release);
        producer_index_ ^= 1U;
        return VideoHandoffStatus::ok;
    }

    // Returned data is immutable and valid until this consumer calls release.
    // Repeated peek calls return the same packet; producer cannot overwrite it.
    // release invalidates every borrowed reference/span into that packet: finish
    // any synchronous use or independent copy first. No stale-pointer lifetime
    // guard is provided, and async users must not retain these borrowed bytes.
    [[nodiscard]] const Packet* peek() const noexcept
    {
        const auto& slot = slots_[consumer_index_];
        return slot.ready.load(std::memory_order_acquire) ? &slot.packet : nullptr;
    }

    // May discard a ready packet even without peek. False means queue empty.
    [[nodiscard]] bool release() noexcept
    {
        auto& slot = slots_[consumer_index_];
        if (!slot.ready.load(std::memory_order_acquire))
            return false;
        slot.ready.store(false, std::memory_order_release);
        consumer_index_ ^= 1U;
        return true;
    }

    [[nodiscard]] std::size_t required_bytes() const noexcept { return required_bytes_; }
    [[nodiscard]] std::size_t packed_bytes() const noexcept { return packed_bytes_; }

private:
    static_assert(std::atomic<bool>::is_always_lock_free,
                  "The capture handoff requires lock-free boolean atomics");
    struct alignas(64) Slot {
        Packet packet;
        std::atomic<bool> ready{false};
    };
    std::array<Slot, 2> slots_;
    std::size_t width_ = 0;
    std::size_t rows_ = 0;
    std::size_t stride_ = 0;
    std::size_t required_bytes_ = 0;
    std::size_t packed_bytes_ = 0;
    alignas(64) std::size_t producer_index_ = 0;
    alignas(64) std::size_t consumer_index_ = 0;
};

} // namespace avsync
