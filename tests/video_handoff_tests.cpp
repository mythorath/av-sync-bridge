// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/video_handoff.hpp"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
unsigned checks = 0;
void check(bool value, const char* expression, int line)
{
    ++checks;
    if (!value)
        throw std::runtime_error(std::string("line ") + std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)

using Handoff = avsync::Nv12VideoHandoff;
using S = avsync::VideoHandoffStatus;

bool invalid_dimensions(std::uint32_t width, std::uint32_t height, std::uint32_t stride)
{
    try {
        Handoff handoff(width, height, stride);
        return false;
    } catch (const std::invalid_argument&) {
        return true;
    }
}

void test_dimensions_and_limits()
{
    CHECK(invalid_dimensions(0, 2, 2));
    CHECK(invalid_dimensions(2, 0, 2));
    CHECK(invalid_dimensions(1, 2, 2));
    CHECK(invalid_dimensions(2, 1, 2));
    CHECK(invalid_dimensions(3842, 2160, 3842));
    CHECK(invalid_dimensions(3840, 2162, 3840));
    CHECK(invalid_dimensions(4, 2, 2));
    CHECK(invalid_dimensions(2, 2, 3));
    constexpr auto maximum = std::numeric_limits<std::uint32_t>::max();
    CHECK(invalid_dimensions(maximum, maximum, maximum));
    CHECK(invalid_dimensions(3840, 2160, maximum - 1));
    CHECK(invalid_dimensions(2, 2, 11'184'812));
    Handoff boundary(2, 2, 11'184'810);
    CHECK(boundary.required_bytes() == Handoff::maximum_payload_bytes - 2);
    CHECK(boundary.packed_bytes() == 6);
    Handoff maximum_image(3840, 2160, 3840);
    CHECK(maximum_image.required_bytes() == 12'441'600);
    CHECK(maximum_image.packed_bytes() == maximum_image.required_bytes());
}

void test_padded_packing_and_ownership()
{
    constexpr std::size_t width = 4;
    constexpr std::size_t height = 4;
    constexpr std::size_t stride = 8;
    Handoff handoff(width, height, stride);
    CHECK(handoff.required_bytes() == 48);
    CHECK(handoff.packed_bytes() == 24);
    std::vector<std::byte> source(48, std::byte{0xee});
    std::vector<std::uint8_t> expected(24);
    for (std::size_t row = 0; row < height + height / 2; ++row) {
        for (std::size_t column = 0; column < width; ++column) {
            const auto value = static_cast<std::uint8_t>(row * 17 + column);
            source[row * stride + column] = static_cast<std::byte>(value);
            expected[row * width + column] = value;
        }
    }
    CHECK(handoff.try_copy(source, 123'456'789) == S::ok);
    const auto* packet = handoff.peek();
    CHECK(packet != nullptr);
    CHECK(packet->pixels == expected);
    CHECK(packet->capture_ns == 123'456'789);
    CHECK(handoff.peek() == packet);
    // Captured bytes belong to the handoff after try_copy returns.
    for (auto& value : source)
        value = std::byte{0};
    CHECK(packet->pixels == expected);
    CHECK(handoff.release());
    CHECK(handoff.peek() == nullptr);
    CHECK(!handoff.release());
}

void test_fifo_full_reuse_and_invalid()
{
    Handoff handoff(2, 2, 2);
    std::vector<std::byte> a(6, std::byte{0x11});
    std::vector<std::byte> b(6, std::byte{0x22});
    std::vector<std::byte> c(6, std::byte{0x33});
    CHECK(handoff.peek() == nullptr);
    CHECK(!handoff.release());
    CHECK(handoff.try_copy({}, 10) == S::invalid);
    CHECK(handoff.try_copy(std::span(a).first(5), 10) == S::invalid);
    CHECK(handoff.try_copy(a, -1) == S::invalid);
    CHECK(handoff.peek() == nullptr);
    CHECK(handoff.try_copy(a, 0) == S::ok);
    const auto* first = handoff.peek();
    const auto* first_storage = first->pixels.data();
    const auto first_capacity = first->pixels.capacity();
    CHECK(handoff.try_copy(b, std::numeric_limits<std::int64_t>::max()) == S::ok);
    CHECK(handoff.try_copy(c, 2) == S::full);
    CHECK(first->capture_ns == 0 && first->pixels[0] == 0x11);
    CHECK(handoff.try_copy(c, -1) == S::invalid);
    CHECK(handoff.release());
    const auto* second = handoff.peek();
    const auto* second_storage = second->pixels.data();
    const auto second_capacity = second->pixels.capacity();
    CHECK(second != first);
    CHECK(second->capture_ns == std::numeric_limits<std::int64_t>::max());
    CHECK(second->pixels == std::vector<std::uint8_t>(6, 0x22));
    CHECK(handoff.try_copy(c, 2) == S::ok);
    CHECK(handoff.peek() == second); // The ready consumer packet is not replaced.
    CHECK(handoff.try_copy(a, 3) == S::full);
    CHECK(handoff.release());
    CHECK(handoff.peek() == first);
    CHECK(handoff.peek()->capture_ns == 2);
    CHECK(handoff.peek()->pixels == std::vector<std::uint8_t>(6, 0x33));
    CHECK(handoff.release());
    CHECK(handoff.peek() == nullptr);

    for (std::int64_t index = 0; index < 10'000; ++index) {
        CHECK(handoff.try_copy(a, index) == S::ok);
        const auto* ready = handoff.peek();
        CHECK(ready != nullptr && ready->capture_ns == index);
        // Both vectors retain their allocated storage and capacity on reuse.
        CHECK((ready == first && ready->pixels.data() == first_storage && ready->pixels.capacity() == first_capacity) ||
              (ready == second && ready->pixels.data() == second_storage && ready->pixels.capacity() == second_capacity));
        CHECK(handoff.release());
    }
    std::vector<std::byte> oversized(Handoff::maximum_payload_bytes + 1);
    CHECK(handoff.try_copy(oversized, 10) == S::invalid);
    CHECK(handoff.peek() == nullptr);
}

void test_trailing_padding_and_all_bytes()
{
    // Last-row padding is required by this adapter's conservative layout bound;
    // extra reported payload bytes are allowed but never copied into packed data.
    Handoff handoff(2, 2, 4);
    std::vector<std::byte> source(16, std::byte{0xff});
    const std::vector<std::uint8_t> expected{0, 1, 2, 3, 4, 5};
    for (std::size_t row = 0; row < 3; ++row) {
        source[row * 4] = static_cast<std::byte>(row * 2);
        source[row * 4 + 1] = static_cast<std::byte>(row * 2 + 1);
    }
    CHECK(handoff.try_copy(std::span(source).first(11), 1) == S::invalid);
    CHECK(handoff.try_copy(source, 1) == S::ok);
    CHECK(handoff.peek()->pixels == expected);
    CHECK(handoff.release());
    CHECK(handoff.try_copy(std::span(source).first(12), 2) == S::ok);
    CHECK(handoff.peek()->pixels == expected);
    CHECK(handoff.release());
}

void test_borrow_blocks_producer_reuse()
{
    Handoff handoff(4, 4, 4);
    std::vector<std::byte> first_source(24, std::byte{0x19});
    std::vector<std::byte> second_source(24, std::byte{0x29});
    std::vector<std::byte> third_source(24, std::byte{0x39});
    CHECK(handoff.try_copy(first_source, 1) == S::ok);
    CHECK(handoff.try_copy(second_source, 2) == S::ok);
    const auto* borrowed = handoff.peek();
    CHECK(borrowed != nullptr);
    const auto owned_copy = borrowed->pixels;
    std::uint32_t full_count = 0;
    std::jthread producer([&] {
        for (std::uint32_t attempt = 0; attempt < 10'000; ++attempt)
            if (handoff.try_copy(third_source, 3) == S::full)
                ++full_count;
    });
    producer.join();
    CHECK(full_count == 10'000);
    CHECK(handoff.peek() == borrowed);
    CHECK(borrowed->capture_ns == 1);
    CHECK(borrowed->pixels == owned_copy);
    CHECK(handoff.release());
    // The raw borrowed pointer is no longer dereferenced. The independent copy
    // stays owned even after the producer reuses the released slot.
    CHECK(handoff.try_copy(third_source, 3) == S::ok);
    CHECK(owned_copy == std::vector<std::uint8_t>(24, 0x19));
    CHECK(handoff.peek()->capture_ns == 2);
    CHECK(handoff.release());
    CHECK(handoff.peek()->capture_ns == 3);
    CHECK(handoff.peek()->pixels == std::vector<std::uint8_t>(24, 0x39));
    CHECK(handoff.release());
}

void test_two_thread_publication()
{
    Handoff handoff(8, 4, 12);
    constexpr std::int64_t count = 50'000;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    std::atomic<bool> failed{false};
    std::atomic<bool> stop{false};
    std::int64_t produced = 0;
    std::int64_t consumed = 0;
    const auto expired = [&] {
        return stop.load(std::memory_order_relaxed) ||
               std::chrono::steady_clock::now() > deadline;
    };
    std::jthread producer([&] {
        std::vector<std::byte> source(72, std::byte{0xff});
        for (std::int64_t frame = 1; frame <= count; ++frame) {
            for (std::size_t row = 0; row < 6; ++row)
                for (std::size_t column = 0; column < 8; ++column)
                    source[row * 12 + column] = static_cast<std::byte>((frame + static_cast<std::int64_t>(row * 8 + column)) % 251);
            while (true) {
                if (expired()) {
                    failed.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                const auto status = handoff.try_copy(source, frame);
                if (status == S::ok)
                    break;
                if (status != S::full) {
                    failed.store(true, std::memory_order_relaxed);
                    stop.store(true, std::memory_order_relaxed);
                    return;
                }
                std::this_thread::yield();
            }
            ++produced;
        }
    });
    std::jthread consumer([&] {
        for (std::int64_t frame = 1; frame <= count;) {
            if (expired()) {
                failed.store(true, std::memory_order_relaxed);
                stop.store(true, std::memory_order_relaxed);
                return;
            }
            const auto* packet = handoff.peek();
            if (!packet) {
                std::this_thread::yield();
                continue;
            }
            bool valid = packet->capture_ns == frame && packet->pixels.size() == 48 && handoff.peek() == packet;
            for (std::size_t byte = 0; valid && byte < packet->pixels.size(); ++byte)
                valid = packet->pixels[byte] == static_cast<std::uint8_t>((frame + static_cast<std::int64_t>(byte)) % 251);
            if (!valid || !handoff.release()) {
                failed.store(true, std::memory_order_relaxed);
                stop.store(true, std::memory_order_relaxed);
                return;
            }
            ++consumed;
            ++frame;
        }
    });
    producer.join();
    consumer.join();
    CHECK(!failed.load());
    CHECK(produced == count);
    CHECK(consumed == count);
    CHECK(handoff.peek() == nullptr);
    CHECK(!handoff.release());
}
} // namespace

int main()
{
    try {
        test_dimensions_and_limits();
        test_padded_packing_and_ownership();
        test_fifo_full_reuse_and_invalid();
        test_trailing_padding_and_all_bytes();
        test_borrow_blocks_producer_reuse();
        test_two_thread_publication();
        std::cout << "video-handoff tests: " << checks
                  << " checks and 50000 threaded frames passed; no hardware or media\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
