# SPDX-License-Identifier: GPL-2.0-or-later
# Offline only: invalid arguments or a pipe already stopped/closed before spawn.
# No accepted, active-control invocation may initialize GStreamer or open UDP.
if(NOT DEFINED EXECUTABLE OR NOT EXISTS "${EXECUTABLE}")
  message(FATAL_ERROR "A built generated network sender is required")
endif()
if(NOT DEFINED PYTHON_EXECUTABLE OR NOT EXISTS "${PYTHON_EXECUTABLE}")
  message(FATAL_ERROR "Python is required to prefill a real control pipe before spawn")
endif()

foreach(case IN ITEMS missing_generated zero_seconds long_seconds zero_role bad_role
    duplicate_ports zero_clock zero_session duplicate_seconds duplicate_generated
    duplicate_control noncanonical_session invalid_host)
  set(generated --generated-audio)
  set(seconds 30)
  set(role 1)
  set(clock 23)
  set(session 17)
  set(rtp 41001)
  set(host 127.0.0.1)
  set(extra)
  if(case STREQUAL "missing_generated")
    set(generated)
  elseif(case STREQUAL "zero_seconds")
    set(seconds 0)
  elseif(case STREQUAL "long_seconds")
    set(seconds 181)
  elseif(case STREQUAL "zero_role")
    set(role 0)
  elseif(case STREQUAL "bad_role")
    set(role 3)
  elseif(case STREQUAL "duplicate_ports")
    set(rtp 41000)
  elseif(case STREQUAL "zero_clock")
    set(clock 0)
  elseif(case STREQUAL "zero_session")
    set(session 0)
  elseif(case STREQUAL "duplicate_seconds")
    set(extra --seconds 30)
  elseif(case STREQUAL "duplicate_generated")
    set(extra --generated-audio)
  elseif(case STREQUAL "duplicate_control")
    set(extra --control-stdin --control-stdin)
  elseif(case STREQUAL "noncanonical_session")
    set(session 017)
  elseif(case STREQUAL "invalid_host")
    set(host 224.0.0.1)
  endif()
  execute_process(COMMAND "${EXECUTABLE}" ${generated} --host "${host}"
    --clock-port 41000 --rtp-port "${rtp}" --rtcp-port 41002
    --clock-epoch "${clock}" --sender-session "${session}"
    --seconds "${seconds}" --fixture-id "${role}" ${extra}
    TIMEOUT 3 RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
  string(REPLACE "\r\n" "\n" out "${out}")
  string(REPLACE "\r\n" "\n" err "${err}")
  if(NOT code STREQUAL "2" OR NOT out STREQUAL "" OR
      NOT err STREQUAL "Invalid generated fixture arguments; no network or devices opened.\n")
    message(FATAL_ERROR "Case ${case} did not return the exact argument-only rejection")
  endif()
endforeach()

# INPUT_FILE would be a regular file and therefore cannot test StdinControl's
# pipe contract. Fill/close both pipes before Popen; no producer race is allowed.
set(pipe_checks [=[
import json
import os
import subprocess
import sys

args = [sys.argv[1], '--generated-audio', '--host', '127.0.0.1',
        '--clock-port', '41000', '--rtp-port', '41001', '--rtcp-port', '41002',
        '--clock-epoch', '23', '--sender-session', '17', '--seconds', '30',
        '--fixture-id', '1', '--control-stdin']
zero = ('generated_packets', 'generated_frames', 'generated_markers',
        'mapped_packets', 'rtp_packets_at_output', 'anchored_rtp_packets',
        'sender_reports', 'anchor_transport_errors', 'queue_overflows',
        'clock_loss_count')
for content, status, code in ((b'AVSYNC_STOP\n', 'control_stopped', 0),
                              (b'', 'control_eof', 1)):
    read_fd, write_fd = os.pipe()
    try:
        if content:
            if os.write(write_fd, content) != len(content):
                raise RuntimeError('Incomplete prefilled control pipe')
    finally:
        os.close(write_fd)
    try:
        child = subprocess.Popen(args, stdin=read_fd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
    finally:
        os.close(read_fd)
    try:
        out, err = child.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.communicate(timeout=3)
        raise RuntimeError('Early control cancellation exceeded its bound') from None
    if child.returncode != code or err or len(out) > 4096:
        raise RuntimeError('Unexpected early control cancellation result')
    lines = out.decode('ascii').splitlines()
    if len(lines) != 1 or not lines[0].startswith('{'):
        raise RuntimeError('Startup output escaped the early control gate')
    result = json.loads(lines[0])
    if (result.get('schema') != 1 or result.get('status') != status or
            result.get('generated_audio') is not True or
            result.get('reason') != 'none' or
            result.get('clock_usable') is not False or
            result.get('original_anchors_transmitted') is not False or
            result.get('media_verified') is not False or
            any(type(result.get(key)) is not int or result[key] != 0 for key in zero)):
        raise RuntimeError('Early cancellation must have no media or clock activity')
print('Prefilled STOP and EOF stayed before network initialization')
]=])
execute_process(COMMAND "${PYTHON_EXECUTABLE}" -c "${pipe_checks}" "${EXECUTABLE}"
  TIMEOUT 15 RESULT_VARIABLE code OUTPUT_VARIABLE out ERROR_VARIABLE err)
if(NOT code STREQUAL "0" OR NOT err STREQUAL "")
  message(FATAL_ERROR "Prefilled control-pipe checks failed: ${err}")
endif()
message(STATUS "Generated sender offline argument and early-control checks passed")
