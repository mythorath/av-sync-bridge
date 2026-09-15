# SPDX-License-Identifier: GPL-2.0-or-later
add_executable(avsync-core-tests core_tests.cpp)
target_link_libraries(avsync-core-tests PRIVATE avsync_core)
add_test(NAME avsync-core-tests COMMAND avsync-core-tests)

add_executable(avsync-video-timing-tests video_timing_tests.cpp)
target_link_libraries(avsync-video-timing-tests PRIVATE avsync_core)
add_test(NAME avsync-video-timing-tests COMMAND avsync-video-timing-tests)
find_package(Threads REQUIRED)
add_executable(avsync-video-handoff-tests video_handoff_tests.cpp)
target_link_libraries(avsync-video-handoff-tests PRIVATE avsync_core Threads::Threads)
add_test(NAME avsync-video-handoff-tests COMMAND avsync-video-handoff-tests)

add_executable(avsync-capture-window-tests capture_window_tests.cpp)
target_link_libraries(avsync-capture-window-tests PRIVATE avsync_core)
add_test(NAME avsync-capture-window-tests COMMAND avsync-capture-window-tests)

add_executable(avsync-audio-anchor-tests audio_anchor_tests.cpp)
target_link_libraries(avsync-audio-anchor-tests PRIVATE avsync_core)
add_test(NAME avsync-audio-anchor-tests COMMAND avsync-audio-anchor-tests)

if(TARGET avsync_asrc)
    add_executable(avsync-asrc-tests asrc_tests.cpp)
    target_link_libraries(avsync-asrc-tests PRIVATE avsync_asrc PkgConfig::SAMPLERATE)
    add_test(NAME avsync-asrc-tests COMMAND avsync-asrc-tests)
endif()

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
