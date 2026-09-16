// SPDX-License-Identifier: GPL-2.0-or-later
// Generated control only: no devices, network, IPC media or OBS.
#include "avsync/process_control.hpp"
#include <iostream>
#include <thread>
int main() {
    try {
        avsync::process::StdinControl control(true);
        std::cout << "CONTROL_PIPE_READY\n" << std::flush;
        while (control.poll() == avsync::process::ControlState::active)
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::cout << avsync::process::state_name(control.state()) << '\n';
        return control.state() == avsync::process::ControlState::stopped ? 0 : 1;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 2; }
}
