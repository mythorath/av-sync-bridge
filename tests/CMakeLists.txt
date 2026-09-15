# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(avsync-core-tests core_tests.cpp)
target_link_libraries(avsync-core-tests PRIVATE avsync_core)
add_test(NAME avsync-core-tests COMMAND avsync-core-tests)

add_executable(avsync-capture-window-tests capture_window_tests.cpp)
target_link_libraries(avsync-capture-window-tests PRIVATE avsync_core)
add_test(NAME avsync-capture-window-tests COMMAND avsync-capture-window-tests)

if(TARGET avsync_gst_clock)
    add_executable(avsync-network-clock-tests network_clock_tests.cpp)
    target_link_libraries(avsync-network-clock-tests PRIVATE avsync_gst_clock)
    add_test(NAME avsync-network-clock-tests COMMAND avsync-network-clock-tests)
endif()

if(AVSYNC_BUILD_IPC AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/ipc_tests.cpp")
    add_executable(avsync-ipc-tests ipc_tests.cpp)
    target_link_libraries(avsync-ipc-tests PRIVATE avsync_ipc)
    target_compile_definitions(avsync-ipc-tests PRIVATE AVSYNC_IPC_TEST_HOOKS)
    add_test(NAME avsync-ipc-tests COMMAND avsync-ipc-tests)
endif()
