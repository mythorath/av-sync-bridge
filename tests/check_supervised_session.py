# SPDX-License-Identifier: GPL-2.0-or-later
"""Offline native CLI checks: invalid arguments or pre-cancelled control pipes.

No active invocation, socket, endpoint or real capture device is requested.
The accepted upper bound is tested with STOP/EOF already in place before Popen.
"""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def command(executable, kind, runtime):
    if kind == "video":
        return [executable, "--capture", "--device", "/dev/avsync-never-open",
                "--runtime-dir", str(runtime)]
    base = [executable, "--clock-port", "58020", "--rtp-port", "58021", "--rtcp-port", "58022"]
    if kind == "sender":
        return base + ["--loopback", "--host", "127.0.0.1", "--clock-epoch", "23", "--sender-session", "17"]
    return base + ["--bind", "127.0.0.1", "--peer", "127.0.0.1", "--expect-anchors", "--expect-sender-session", "17"]


def run_child(argv, content=None):
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    read_fd = None
    if content is not None:
        read_fd, write_fd = os.pipe()
        try:
            if content and os.write(write_fd, content) != len(content):
                raise RuntimeError("short_prefilled_control")
        finally:
            os.close(write_fd)
    try:
        child = subprocess.Popen(argv, stdin=read_fd if read_fd is not None else subprocess.DEVNULL,
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, **options)
    finally:
        if read_fd is not None:
            os.close(read_fd)
    try:
        out, err = child.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        child.kill()
        child.communicate(timeout=3)
        raise RuntimeError("offline_child_timeout") from None
    if len(out) > 65536 or len(err) > 4096:
        raise RuntimeError("offline_output_limit")
    return child.returncode, out.decode("ascii"), err.decode("ascii")


def check(executable, kind):
    with tempfile.TemporaryDirectory(prefix="avsync-session-arguments-") as directory:
        runtime = Path(directory) / "never-created"
        base = command(executable, kind, runtime)
        rejection = 1 if kind == "receiver" else 2
        invalid = [
            ["--session-seconds", "0", "--control-stdin"],
            ["--session-seconds", "43201", "--control-stdin"],
            ["--session-seconds", "43200"],
            ["--session-seconds", "181", "--seconds", "1", "--control-stdin"],
            ["--seconds", "1", "--session-seconds", "181", "--control-stdin"],
            ["--session-seconds", "181", "--session-seconds", "181", "--control-stdin"],
            ["--seconds", "0"], ["--seconds", "181"],
        ]
        if kind == "receiver":
            invalid += [["--session-seconds", "181", "--control-stdin", "--recover-desktop-ipc"],
                        ["--session-seconds", "181", "--control-stdin", "--clock-pause-after", "10"]]
        if kind == "video":
            invalid += [["--session-seconds", "181", "--control-stdin", "--trace-markers"]]
        if kind == "receiver":
            numeric = "Invalid numeric argument\n"
            repeat = "Duration options are mutually exclusive and cannot repeat\n"
            expected_errors = [numeric, numeric, "Supervised sessions require pinned stdin control\n",
                repeat, repeat, repeat, numeric, numeric,
                "Supervised sessions cannot use recovery or clock-pause fixture options\n",
                "Supervised sessions cannot use recovery or clock-pause fixture options\n"]
        elif kind == "video":
            numeric = "invalid numeric argument\n"
            expected_errors = [numeric, numeric, "supervised sessions require stdin control\n",
                "repeated argument\n", "repeated argument\n", "repeated argument\n", numeric, numeric,
                "supervised sessions cannot use finite marker tracing\n"]
        for index, suffix in enumerate(invalid):
            code, out, err = run_child(base + suffix)
            if code != rejection or "READY" in out or "AVSYNC_CONTROL" in out:
                raise RuntimeError("session_argument_rejection_failed_" + str(index))
            if kind == "sender":
                if json.loads(out) != {"schema": 1, "status": "error", "error_stage": "arguments"} or err:
                    raise RuntimeError("sender_rejection_not_argument_only")
            elif out or err.replace("\r\n", "\n") != expected_errors[index]:
                raise RuntimeError("rejection_not_argument_only")
        # Session mode requires a pinned sender identity, not merely a pipe.
        if kind != "video":
            flag = "--sender-session" if kind == "sender" else "--expect-sender-session"
            unpinned = base.copy()
            index = unpinned.index(flag)
            del unpinned[index:index + 2]
            code, out, _ = run_child(unpinned + ["--session-seconds", "181", "--control-stdin"])
            if code != rejection or "READY" in out or "AVSYNC_CONTROL" in out:
                raise RuntimeError("unpinned_session_accepted")
        for duration_flag, duration in (("--seconds", "180"), ("--session-seconds", "1"),
                                        ("--session-seconds", "181"), ("--session-seconds", "43200")):
            for data, expected, expected_code in ((b"AVSYNC_STOP\n", "control_stopped", 0),
                                                   (b"", "control_eof", 1)):
                code, out, err = run_child(base + [duration_flag, duration, "--control-stdin"], data)
                lines = out.splitlines()
                if code != expected_code or err or len(lines) != 1 or not lines[0].startswith("{"):
                    raise RuntimeError("early_session_control_failed")
                report = json.loads(lines[0])
                if report.get("schema") != 1 or report.get("status") != expected:
                    raise RuntimeError("early_session_control_summary_failed")
                if kind == "sender" and (report.get("captured_frames") != 0 or report.get("mapped_packets") != 0):
                    raise RuntimeError("cancelled_sender_captured_media")
                if kind == "video" and (report.get("captured") != 0 or report.get("published") != 0 or
                                         report.get("runtime_cleaned") is not True):
                    raise RuntimeError("cancelled_video_captured_media")
                if runtime.exists():
                    raise RuntimeError("cancelled_video_created_runtime")
    print("Offline supervised duration and pre-cancelled control checks passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--executable", required=True)
    parser.add_argument("--kind", choices=("sender", "receiver", "video"), required=True)
    args = parser.parse_args()
    check(args.executable, args.kind)


if __name__ == "__main__":
    main()
