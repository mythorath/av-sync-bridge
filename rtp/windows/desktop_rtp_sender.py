#!/usr/bin/env python3
"""Always-on Windows desktop audio sender: WASAPI loopback -> RTP L24 -> Linux/OBS.

Production replacement for the leased sender/receiver pair (apps/windows_sender.cpp +
apps/network_receiver.cpp). The receiver is PipeWire's own rtp-source (see
rtp/linux/), which absorbs jitter, clock drift, silence and sender restarts
without exiting.

Design rules for this side:
- Nothing exits because of a timing hiccup. There are no deadlines to miss.
- One GStreamer child does capture, downmix and send. If it exits, or stops
  producing packets, it is restarted after a short backoff and the reason is logged.
- Every packet is also copied to 127.0.0.1:PROBE_PORT and counted here, so
  liveness is measured on what is actually sent, not on whether a process exists.
- Children live in a kill-on-close job object: if this supervisor dies, so do they.
- Every child is created with CREATE_NO_WINDOW (this runs under pythonw).

Stop: create <state-dir>/rtp-stop.flag (a stop script can do this) or end this process.

Usage (pythonw on Windows, normally from a scheduled task):
    desktop_rtp_sender.py --target 192.0.2.10:46000
        [--gst-launch C:/gstreamer/1.0/msvc_x86_64/bin/gst-launch-1.0.exe] [--state-dir DIR]
"""
from __future__ import annotations

import argparse
import ctypes
import ctypes.wintypes as wt
import json
import os
import re
import socket
import subprocess
import time
from pathlib import Path

BASE = Path(__file__).resolve().parent
# Overridden from the command line in configure(); defaults keep the module importable.
STATE = BASE / "state"
LOG = STATE / "rtp-sender.log"
STATUS = STATE / "rtp-status.json"
HISTORY = STATE / "rtp-history.jsonl"
STOP_FLAG = STATE / "rtp-stop.flag"
GST_LAUNCH = "gst-launch-1.0.exe"     # GStreamer 1.24+ (wasapi2src, rtpL24pay)
TARGET = ""                          # host:port of the PipeWire rtp-source (required argument)
PROBE_PORT = 46099                   # local copy of every packet, for liveness
PACKET_NS = 2_500_000                # 2.5 ms per packet = 120 frames = 400 packets/s
NO_WINDOW = 0x08000000
ABOVE_NORMAL = 0x00008000
STALL_SECONDS = 3.0                  # a running sender that sends nothing for this long is restarted
STARTUP_GRACE_SECONDS = 15.0         # first packet must arrive within this
BACKOFF = (1, 2, 5, 10)
HEALTHY_SECONDS = 60
LOG_MAX_BYTES = 2_000_000
HISTORY_LINES = 500

K = 0.7071067811865476
# GStreamer channel-position bit -> (left, right) weight. Same policy as the old
# avsync sender so OBS levels do not change: LFE dropped, centre/rear/side at -3 dB,
# one common gain with 10% headroom.
WEIGHTS = {0: (1, 0), 1: (0, 1), 2: (K, K), 3: (0, 0), 4: (K, 0), 5: (0, K),
           6: (1, 0), 7: (0, 1), 8: (0, 0), 9: (0.5, 0.5), 10: (K, 0), 11: (0, K)}

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.CreateMutexW.restype = wt.HANDLE
kernel32.CreateMutexW.argtypes = (wt.LPVOID, wt.BOOL, wt.LPCWSTR)
kernel32.CreateJobObjectW.restype = wt.HANDLE
kernel32.CreateJobObjectW.argtypes = (wt.LPVOID, wt.LPCWSTR)
kernel32.SetInformationJobObject.argtypes = (wt.HANDLE, ctypes.c_int, wt.LPVOID, wt.DWORD)
kernel32.AssignProcessToJobObject.argtypes = (wt.HANDLE, wt.HANDLE)


class _IoCounters(ctypes.Structure):
    _fields_ = [(name, ctypes.c_ulonglong) for name in ("r", "w", "o", "rb", "wb", "ob")]


class _BasicLimits(ctypes.Structure):
    _fields_ = [("PerProcessUserTimeLimit", ctypes.c_int64), ("PerJobUserTimeLimit", ctypes.c_int64),
                ("LimitFlags", wt.DWORD), ("MinimumWorkingSetSize", ctypes.c_size_t),
                ("MaximumWorkingSetSize", ctypes.c_size_t), ("ActiveProcessLimit", wt.DWORD),
                ("Affinity", ctypes.c_size_t), ("PriorityClass", wt.DWORD), ("SchedulingClass", wt.DWORD)]


class _ExtendedLimits(ctypes.Structure):
    _fields_ = [("BasicLimitInformation", _BasicLimits), ("IoInfo", _IoCounters),
                ("ProcessMemoryLimit", ctypes.c_size_t), ("JobMemoryLimit", ctypes.c_size_t),
                ("PeakProcessMemoryUsed", ctypes.c_size_t), ("PeakJobMemoryUsed", ctypes.c_size_t)]


def kill_on_close_job():
    job = kernel32.CreateJobObjectW(None, None)
    if not job:
        return None
    info = _ExtendedLimits()
    info.BasicLimitInformation.LimitFlags = 0x2000  # JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
    if not kernel32.SetInformationJobObject(job, 9, ctypes.byref(info), ctypes.sizeof(info)):
        return None
    return job


def log(message: str) -> None:
    line = time.strftime("%Y-%m-%d %H:%M:%S ") + message + "\n"
    try:
        with open(LOG, "a", encoding="utf-8") as handle:
            handle.write(line)
    except OSError:
        pass


def rotate_log() -> None:
    try:
        if LOG.exists() and LOG.stat().st_size > LOG_MAX_BYTES:
            old = LOG.with_suffix(".log.1")
            old.unlink(missing_ok=True)
            LOG.rename(old)
    except OSError:
        pass


def append_history(record: dict) -> None:
    try:
        lines = HISTORY.read_text(encoding="utf-8").splitlines() if HISTORY.exists() else []
        lines.append(json.dumps(record, separators=(",", ":")))
        HISTORY.write_text("\n".join(lines[-HISTORY_LINES:]) + "\n", encoding="utf-8")
    except OSError:
        pass


def write_status(**fields) -> None:
    record = {"schema": 1, "pid": os.getpid(), "updated": time.time(), **fields}
    tmp = STATUS.with_suffix(".tmp")
    for _ in range(5):  # a reader holding the file makes os.replace fail briefly on Windows
        try:
            tmp.write_text(json.dumps(record), encoding="utf-8")
            os.replace(tmp, STATUS)
            return
        except OSError:
            time.sleep(0.05)


def probe_format() -> tuple[int, int, int]:
    """Channels, GStreamer channel mask and rate of the current default render endpoint."""
    result = subprocess.run(
        [GST_LAUNCH, "-v", "wasapi2src", "loopback=true", "low-latency=true", "num-buffers=1", "!", "fakesink"],
        capture_output=True, text=True, timeout=20, stdin=subprocess.DEVNULL, creationflags=NO_WINDOW)
    match = re.search(r"wasapi2src\d*\.GstPad:src: caps = (audio/x-raw[^\r\n]*)", result.stdout)
    if not match:
        raise RuntimeError("format probe produced no caps: " + (result.stderr or result.stdout)[-300:].strip())
    caps = match.group(1)
    channels = int(re.search(r"channels=\(int\)(\d+)", caps).group(1))
    rate = int(re.search(r"rate=\(int\)(\d+)", caps).group(1))
    mask_match = re.search(r"channel-mask=\(bitmask\)0x([0-9a-fA-F]+)", caps)
    mask = int(mask_match.group(1), 16) if mask_match else 0
    return channels, mask, rate


def downmix_matrix(channels: int, mask: int) -> str:
    if channels == 1:
        rows = [[1.0], [1.0]]
    else:
        if channels == 2 and mask == 0:
            mask = 3
        bits = [bit for bit in range(64) if mask >> bit & 1]
        if len(bits) != channels or any(bit not in WEIGHTS for bit in bits):
            raise RuntimeError(f"unsupported speaker layout: {channels} ch, mask 0x{mask:x}")
        rows = [[WEIGHTS[bit][side] for bit in bits] for side in (0, 1)]
    gain = 0.9 / max(sum(abs(value) for value in row) for row in rows)
    return "<" + ",".join("<" + ",".join(f"(float){value * gain:.9f}" for value in row) + ">"
                          for row in rows) + ">"


def sender_argv(channels: int, mask: int) -> list[str]:
    source_caps = f"audio/x-raw,channels={channels}" + (f",channel-mask=(bitmask)0x{mask:x}" if mask else "")
    return [GST_LAUNCH,
            "wasapi2src", "loopback=true", "low-latency=true", "continue-on-error=true", "!",
            source_caps, "!",
            "audioconvert", "mix-matrix=" + downmix_matrix(channels, mask),
            "dithering=none", "noise-shaping=none", "!",
            "audioresample", "!",
            "audio/x-raw,format=S24BE,rate=48000,channels=2,channel-mask=(bitmask)0x3,layout=interleaved", "!",
            "rtpL24pay", "pt=96", f"min-ptime={PACKET_NS}", f"max-ptime={PACKET_NS}", "!",
            "multiudpsink", f"clients={TARGET},127.0.0.1:{PROBE_PORT}", "sync=false", "async=false"]


def stop_requested() -> bool:
    return STOP_FLAG.exists()


def sleep_unless_stopped(seconds: float, **status) -> bool:
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        if stop_requested():
            return False
        write_status(state="restarting", retry_in=round(end - time.monotonic(), 1), **status)
        time.sleep(min(0.5, max(0.0, end - time.monotonic())))
    return True


def run_once(job, probe: socket.socket, restarts: int) -> tuple[str, float]:
    """Run one sender child until it fails or a stop is requested. Returns (reason, seconds)."""
    channels, mask, rate = probe_format()
    fmt = f"{channels}ch {rate}Hz mask=0x{mask:x}"
    rotate_log()
    log(f"starting sender: endpoint {fmt}")
    with open(LOG, "ab") as child_log:
        child = subprocess.Popen(sender_argv(channels, mask), stdin=subprocess.DEVNULL, stdout=child_log,
                                 stderr=child_log, creationflags=NO_WINDOW | ABOVE_NORMAL)
    if job:
        kernel32.AssignProcessToJobObject(job, int(child._handle))
    started = time.monotonic()
    last_packet = None
    window_start, window_packets = started, 0
    total = 0
    try:
        while True:
            time.sleep(0.25)
            now = time.monotonic()
            while True:
                try:
                    probe.recv(4096)
                except (BlockingIOError, ConnectionResetError):
                    break
                window_packets += 1
                total += 1
                last_packet = now
            if now - window_start >= 1.0:
                rate_pps = window_packets / (now - window_start)
                window_start, window_packets = now, 0
                write_status(state="running" if last_packet else "starting", sender_pid=child.pid,
                             endpoint=fmt, packets_per_second=round(rate_pps, 1), packets_total=total,
                             restarts=restarts, since=round(now - started, 1))
            if stop_requested():
                return "stop requested", now - started
            code = child.poll()
            if code is not None:
                return f"sender exited (code {code})", now - started
            if last_packet is None and now - started > STARTUP_GRACE_SECONDS:
                return f"no packets within {STARTUP_GRACE_SECONDS:.0f}s of start", now - started
            if last_packet is not None and now - last_packet > STALL_SECONDS:
                return f"no packets for {STALL_SECONDS:.0f}s", now - started
    finally:
        if child.poll() is None:
            child.kill()
            try:
                child.wait(5)
            except subprocess.TimeoutExpired:
                pass


def configure() -> None:
    global STATE, LOG, STATUS, HISTORY, STOP_FLAG, GST_LAUNCH, TARGET
    parser = argparse.ArgumentParser(description="Always-on WASAPI loopback to RTP L24 sender.")
    parser.add_argument("--target", required=True, help="receiver host:port, e.g. 192.0.2.10:46000")
    parser.add_argument("--gst-launch", default=GST_LAUNCH, help="path to gst-launch-1.0.exe")
    parser.add_argument("--state-dir", default=str(STATE), help="status, history and log directory")
    args = parser.parse_args()
    TARGET, GST_LAUNCH = args.target, args.gst_launch
    STATE = Path(args.state_dir)
    LOG, STATUS = STATE / "rtp-sender.log", STATE / "rtp-status.json"
    HISTORY, STOP_FLAG = STATE / "rtp-history.jsonl", STATE / "rtp-stop.flag"


def main() -> int:
    configure()
    STATE.mkdir(parents=True, exist_ok=True)
    mutex = kernel32.CreateMutexW(None, False, "Local\\AVSyncDesktopRtpSender")
    if ctypes.get_last_error() == 183:  # ERROR_ALREADY_EXISTS: another supervisor owns the sender
        return 0
    STOP_FLAG.unlink(missing_ok=True)
    job = kill_on_close_job()
    probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # 400 packets/s drained every 0.25 s would overflow the 64 KB default buffer.
    probe.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4 * 1024 * 1024)
    probe.bind(("127.0.0.1", PROBE_PORT))
    probe.setblocking(False)
    log(f"supervisor started (pid {os.getpid()})")
    restarts, failures_in_row = 0, 0
    try:
        while not stop_requested():
            try:
                reason, seconds = run_once(job, probe, restarts)
            except Exception as error:  # the supervisor itself must never die on a surprise
                reason, seconds = f"error: {error!r}"[:400], 0.0
            if reason == "stop requested":
                break
            log(f"restart {restarts + 1}: {reason} after {seconds:.1f}s")
            append_history({"time": time.strftime("%Y-%m-%d %H:%M:%S"), "restart": restarts + 1,
                            "reason": reason, "ran_seconds": round(seconds, 1)})
            failures_in_row = 0 if seconds >= HEALTHY_SECONDS else failures_in_row + 1
            restarts += 1
            delay = BACKOFF[min(failures_in_row, len(BACKOFF)) - 1] if failures_in_row else 1
            if not sleep_unless_stopped(delay, restarts=restarts, last_restart_reason=reason):
                break
    finally:
        log("supervisor stopped")
        write_status(state="stopped", restarts=restarts)
        STOP_FLAG.unlink(missing_ok=True)
        probe.close()
        del mutex
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
