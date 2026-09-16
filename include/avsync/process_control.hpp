// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <span>
#include <stdexcept>
#include <string_view>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace avsync::process {

enum class ControlState { active, stopped, eof, expired, invalid, read_error };
inline const char* state_name(ControlState state) noexcept {
    switch (state) {
    case ControlState::active: return "active";
    case ControlState::stopped: return "control_stopped";
    case ControlState::eof: return "control_eof";
    case ControlState::expired: return "control_expired";
    case ControlState::invalid: return "control_invalid";
    case ControlState::read_error: return "control_read_error";
    }
    return "control_invalid";
}

// A finite control lease, NOT a clock/media-health gate or authentication.
// A partial line never renews a lease. Terminal states cannot be revived.
// Call tick before reading: bytes delivered after expiry cannot resuscitate it.
class ControlLease {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    static constexpr auto timeout = std::chrono::seconds(5);
    static constexpr std::size_t maximum_line = 32;
    explicit ControlLease(Time now) noexcept : heartbeat_(now), last_poll_(now) {}
    ControlState state() const noexcept { return state_; }
    ControlState tick(Time now) noexcept {
        if (state_ != ControlState::active) return state_;
        if (now < last_poll_) return state_ = ControlState::invalid;
        last_poll_ = now;
        if (now - heartbeat_ >= timeout) state_ = ControlState::expired;
        return state_;
    }
    ControlState consume(std::span<const char> bytes, Time now) noexcept {
        if (tick(now) != ControlState::active) return state_;
        for (const char byte : bytes) {
            if (byte == '\n') {
                const std::string_view line(line_.data(), size_);
                size_ = 0;
                if (line == "AVSYNC_STOP") return state_ = ControlState::stopped;
                if (line != "AVSYNC_KEEPALIVE") return state_ = ControlState::invalid;
                heartbeat_ = now;
            } else {
                if (byte < ' ' || byte > '~' || size_ == line_.size())
                    return state_ = ControlState::invalid;
                line_[size_++] = byte;
            }
        }
        return state_;
    }
    ControlState fail(ControlState state) noexcept {
        if (state_ == ControlState::active && state != ControlState::active)
            state_ = state;
        return state_;
    }
private:
    Time heartbeat_, last_poll_;
    std::array<char, maximum_line> line_{};
    std::size_t size_{};
    ControlState state_{ControlState::active};
};

// Explicit opt-in stdin pipe. Polling never waits for input, allocates or owns a
// reader thread. One owner, at most 1024 bytes per poll. The caller must poll
// during acquisition as well as media processing and perform orderly cleanup
// on any terminal state. A blocked third-party call is not preempted by this.
class StdinControl {
public:
    explicit StdinControl(bool enabled) : enabled_(enabled), lease_(ControlLease::Clock::now()) {
        if (!enabled_) return;
#ifdef _WIN32
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        if (input_ == nullptr || input_ == INVALID_HANDLE_VALUE || GetFileType(input_) != FILE_TYPE_PIPE)
            throw std::runtime_error("Control stdin must be a pipe");
#else
        struct stat info{};
        if (fstat(STDIN_FILENO, &info) != 0 || !S_ISFIFO(info.st_mode))
            throw std::runtime_error("Control stdin must be a pipe");
#endif
    }
    ControlState state() const noexcept { return enabled_ ? lease_.state() : ControlState::active; }
    ControlState poll() noexcept {
        if (!enabled_) return ControlState::active;
        if (lease_.tick(ControlLease::Clock::now()) != ControlState::active) return lease_.state();
        std::array<char, 1024> bytes{};
#ifdef _WIN32
        DWORD available{};
        if (!PeekNamedPipe(input_, nullptr, 0, nullptr, &available, nullptr))
            return lease_.fail(GetLastError() == ERROR_BROKEN_PIPE ? ControlState::eof : ControlState::read_error);
        if (!available) return lease_.state();
        DWORD count{};
        if (!ReadFile(input_, bytes.data(), std::min<DWORD>(available, static_cast<DWORD>(bytes.size())), &count, nullptr))
            return lease_.fail(GetLastError() == ERROR_BROKEN_PIPE ? ControlState::eof : ControlState::read_error);
        if (!count) return lease_.fail(ControlState::eof);
        return lease_.consume(std::span<const char>(bytes.data(), count), ControlLease::Clock::now());
#else
        pollfd descriptor{STDIN_FILENO, POLLIN, 0};
        const int ready = ::poll(&descriptor, 1, 0);
        if (ready < 0) return errno == EINTR ? lease_.state() : lease_.fail(ControlState::read_error);
        if (!ready) return lease_.state();
        if (descriptor.revents & (POLLERR | POLLNVAL)) return lease_.fail(ControlState::read_error);
        if (!(descriptor.revents & (POLLIN | POLLHUP))) return lease_.state();
        const auto count = read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count < 0) return errno == EINTR || errno == EAGAIN ? lease_.state() : lease_.fail(ControlState::read_error);
        if (!count) return lease_.fail(ControlState::eof);
        return lease_.consume(std::span<const char>(bytes.data(), static_cast<std::size_t>(count)), ControlLease::Clock::now());
#endif
    }
private:
    bool enabled_{};
    ControlLease lease_;
#ifdef _WIN32
    HANDLE input_{INVALID_HANDLE_VALUE};
#endif
};

} // namespace avsync::process
