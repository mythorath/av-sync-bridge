// SPDX-License-Identifier: GPL-2.0-or-later
#include "avsync/process_control.hpp"
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
unsigned checks{};
void check(bool value, const char* expression, int line) {
    ++checks;
    if (!value) throw std::runtime_error(std::to_string(line) + ": " + expression);
}
#define CHECK(expression) check(static_cast<bool>(expression), #expression, __LINE__)
using L = avsync::process::ControlLease;
using S = avsync::process::ControlState;
using namespace std::chrono_literals;
const auto start = L::Time{} + 10s;
S consume(L& lease, std::string_view bytes, L::Time when) {
    return lease.consume({bytes.data(), bytes.size()}, when);
}
void test_lease() {
    L lease(start);
    CHECK(lease.tick(start + 4999ms) == S::active);
    CHECK(lease.tick(start + 5s) == S::expired);
    CHECK(consume(lease, "AVSYNC_KEEPALIVE\n", start + 5s) == S::expired);
    CHECK(consume(lease, "AVSYNC_STOP\n", start + 5s) == S::expired);
    L renewed(start);
    CHECK(consume(renewed, "AVSYNC_KEEPALIVE\n", start + 4s) == S::active);
    CHECK(renewed.tick(start + 8s) == S::active);
    CHECK(renewed.tick(start + 9s) == S::expired);
    L boundary(start);
    CHECK(consume(boundary, "AVSYNC_KEEPALIVE\n", start + 5s) == S::expired);
    L backwards(start);
    CHECK(backwards.tick(start - 1ns) == S::invalid);
}
void test_commands() {
    constexpr std::string_view command = "AVSYNC_KEEPALIVE\n";
    for (std::size_t split = 0; split <= command.size(); ++split) {
        L lease(start);
        CHECK(consume(lease, command.substr(0, split), start + 1s) == S::active);
        CHECK(consume(lease, command.substr(split), start + 2s) == S::active);
        CHECK(consume(lease, "AVSYNC_STOP\n", start + 3s) == S::stopped);
        CHECK(consume(lease, command, start + 4s) == S::stopped);
    }
    L fragmented(start);
    CHECK(consume(fragmented, "AVSYNC_KEEPALIVE", start + 4s) == S::active);
    CHECK(consume(fragmented, "\n", start + 5s) == S::expired);
    for (const auto text : {"\n", "AVSYNC_KEEPALIVE\r\n", "STOP\n", " AVSYNC_STOP\n", "AVSYNC_STOP \n"}) {
        L lease(start);
        CHECK(consume(lease, text, start) == S::invalid);
    }
    L excess(start);
    CHECK(consume(excess, std::string(L::maximum_line + 1, 'A'), start) == S::invalid);
    L combined(start);
    CHECK(consume(combined, "AVSYNC_KEEPALIVE\nAVSYNC_KEEPALIVE\nAVSYNC_STOP\n", start) == S::stopped);
    L eof(start);
    CHECK(eof.fail(S::eof) == S::eof);
    CHECK(consume(eof, command, start) == S::eof);
    CHECK(eof.fail(S::read_error) == S::eof);
    avsync::process::StdinControl disabled(false);
    CHECK(disabled.poll() == S::active);
}
}
int main() {
    try { test_lease(); test_commands(); std::cout << checks << " checks passed\n"; return 0; }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
