# SPDX-License-Identifier: GPL-2.0-or-later
# A later device/network failure must not masquerade as argument rejection.
if(NOT DEFINED EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
  message(FATAL_ERROR "A built diagnostic executable is required")
endif()
if(NOT DEFINED CASE_SECONDS OR NOT CASE_SECONDS MATCHES "^(0|181)$")
  message(FATAL_ERROR "Expected the zero or above-maximum duration case")
endif()

if(TEST_KIND STREQUAL "video")
  set(arguments --capture --device /dev/avsync-invalid-test
    --runtime-dir /avsync-invalid-test --seconds "${CASE_SECONDS}")
  set(expected_stdout "")
  set(expected_stderr "invalid numeric argument\n")
elseif(TEST_KIND STREQUAL "sender")
  set(arguments --loopback --host 192.0.2.1
    --clock-port 58000 --rtp-port 58001 --rtcp-port 58002
    --clock-epoch 1 --seconds "${CASE_SECONDS}")
  set(expected_stdout "{\"schema\":1,\"status\":\"error\",\"error_stage\":\"arguments\"}\n")
  set(expected_stderr "")
else()
  message(FATAL_ERROR "Expected TEST_KIND=video or TEST_KIND=sender")
endif()

execute_process(COMMAND "${EXECUTABLE}" ${arguments}
  TIMEOUT 3
  RESULT_VARIABLE exit_status
  OUTPUT_VARIABLE actual_stdout
  ERROR_VARIABLE actual_stderr)
if(NOT exit_status STREQUAL "2")
  message(FATAL_ERROR "Duration must fail during argument parsing with exit 2; got ${exit_status}")
endif()
# Accept native Windows line endings, but no extra messages, READY lines,
# summaries, trailing whitespace, or additional blank lines.
string(REPLACE "\r\n" "\n" actual_stdout "${actual_stdout}")
string(REPLACE "\r\n" "\n" actual_stderr "${actual_stderr}")
if(NOT actual_stdout STREQUAL expected_stdout OR NOT actual_stderr STREQUAL expected_stderr)
  message(FATAL_ERROR "Duration rejection output did not exactly match the argument-only diagnostic")
endif()
