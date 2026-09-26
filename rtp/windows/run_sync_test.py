#!/usr/bin/env python3
"""Hands-off A/V sync pass using the non-repeating reference (tools/av-reference.html).

The reference plays six unique markers (MARKER n/6, each with its own tone from
660 to 1760 Hz), so a measured offset can never be off by a whole marker period
the way a repeating flash/beep test can.

Run it on the Windows machine whose screen the capture card sees:
  1. Serves the reference page on 127.0.0.1 (tools/serve-reference.py).
  2. Opens it in a separate kiosk Chrome on the main monitor, driven over the
     Chrome DevTools protocol on loopback.
  3. Refuses to run if OBS is streaming or recording, or if the desktop audio
     input is not quiet. Switches to the capture scene and starts an OBS recording.
  4. Clicks "Start one pass" as a real user gesture and waits for the page to finish.
  5. Stops the recording, restores scene and record folder, closes that Chrome.
  6. Trims and analyzes the recording on the OBS machine over SSH with
     tools/measure_physical.py, then prints per-marker audio-minus-video offsets,
     the median, the pass gate and a suggested sync offset.

Requires: Python 3.10+, websocket-client, Google Chrome, OpenSSH client, and on the
OBS machine this repository, ffmpeg and NumPy. OBS WebSocket v5 must be enabled.

Example:
  set OBS_WEBSOCKET_PASSWORD=...
  python run_sync_test.py --obs-host 192.0.2.10 --ssh user@192.0.2.10 ^
      --remote-repo /home/user/av-sync-bridge --apply
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import json
import math
import os
import socket
import statistics
import subprocess
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

import websocket  # pip install websocket-client

REPO = Path(__file__).resolve().parents[2]
NO_WINDOW = 0x08000000 if os.name == "nt" else 0
DEVTOOLS_PORT = 9333
REFERENCE_PORT = 58533
PASS_TIMEOUT = 150
QUIET_DB = -50.0


def say(message: str) -> None:
    print(message, flush=True)


class Obs:
    """Minimal OBS WebSocket v5 client (requests plus InputVolumeMeters events)."""

    def __init__(self, host: str, port: int, password: str):
        self.ws = websocket.create_connection(f"ws://{host}:{port}", timeout=10, suppress_origin=True)
        hello = json.loads(self.ws.recv())["d"]
        identify = {"rpcVersion": 1, "eventSubscriptions": 1 << 16}  # InputVolumeMeters
        auth = hello.get("authentication")
        if auth:
            secret = base64.b64encode(hashlib.sha256((password + auth["salt"]).encode()).digest()).decode()
            identify["authentication"] = base64.b64encode(
                hashlib.sha256((secret + auth["challenge"]).encode()).digest()).decode()
        self.ws.send(json.dumps({"op": 1, "d": identify}))
        while json.loads(self.ws.recv())["op"] != 2:
            pass
        self.next_id = 0

    def __call__(self, request: str, **data):
        self.next_id += 1
        rid = str(self.next_id)
        self.ws.send(json.dumps({"op": 6, "d": {"requestType": request, "requestId": rid, "requestData": data}}))
        while True:
            message = json.loads(self.ws.recv())
            if message["op"] == 7 and message["d"]["requestId"] == rid:
                status = message["d"]["requestStatus"]
                if not status["result"]:
                    raise RuntimeError(f"OBS {request}: {status.get('comment', status.get('code'))}")
                return message["d"].get("responseData") or {}

    def peak_db(self, input_name: str, seconds: float) -> float:
        peak, end = -float("inf"), time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                message = json.loads(self.ws.recv())
            except websocket.WebSocketTimeoutException:
                continue
            if message["op"] == 5 and message["d"].get("eventType") == "InputVolumeMeters":
                for item in message["d"]["eventData"]["inputs"]:
                    if item.get("inputName") == input_name:
                        for level in item.get("inputLevelsMul") or []:
                            if len(level) > 1 and level[1] > 0:  # [magnitude, peak, input peak]
                                peak = max(peak, 20 * math.log10(level[1]))
        return peak


class DevTools:
    def __init__(self, url: str):
        # No Origin header: Chrome only accepts DevTools websockets without one (or from an allowed origin).
        self.ws = websocket.create_connection(url, timeout=10, suppress_origin=True)
        self.next_id = 0

    def evaluate(self, expression: str, gesture: bool = False):
        self.next_id += 1
        self.ws.send(json.dumps({"id": self.next_id, "method": "Runtime.evaluate", "params": {
            "expression": expression, "returnByValue": True, "userGesture": gesture}}))
        while True:
            message = json.loads(self.ws.recv())
            if message.get("id") == self.next_id:
                return message.get("result", {}).get("result", {}).get("value")


def port_open(port: int) -> bool:
    with socket.socket() as sock:
        sock.settimeout(0.5)
        return sock.connect_ex(("127.0.0.1", port)) == 0


def find_chrome() -> str:
    for base in (os.environ.get("ProgramFiles", ""), os.environ.get("ProgramFiles(x86)", ""),
                 os.environ.get("LOCALAPPDATA", "")):
        path = Path(base) / "Google" / "Chrome" / "Application" / "chrome.exe"
        if path.exists():
            return str(path)
    raise RuntimeError("Chrome not found")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--obs-host", required=True)
    parser.add_argument("--obs-port", type=int, default=4455)
    parser.add_argument("--obs-password", default=os.environ.get("OBS_WEBSOCKET_PASSWORD", ""))
    parser.add_argument("--audio-input", default="Windows Audio", help="OBS input carrying the desktop audio")
    parser.add_argument("--scene", default="Main4k", help="scene that shows the capture card")
    parser.add_argument("--ssh", required=True, help="user@host of the OBS machine")
    parser.add_argument("--remote-repo", required=True, help="path of this repository on the OBS machine")
    parser.add_argument("--remote-dir", default="/tmp/avsync-sync-tests", help="where OBS writes the test recording")
    parser.add_argument("--reference-dir", default=str(REPO / "tools"), help="folder with av-reference.html")
    parser.add_argument("--results", default=str(Path.cwd() / "sync-tests.jsonl"))
    parser.add_argument("--apply", action="store_true", help="set the suggested offset if the median is off")
    args = parser.parse_args()
    ssh = ["ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8", args.ssh]
    say("AV SYNC TEST (non-repeating reference, automatic)")

    obs = Obs(args.obs_host, args.obs_port, args.obs_password)
    if obs("GetStreamStatus").get("outputActive"):
        say("[STOP] OBS is streaming. The test pattern would go out live; not running.")
        return 2
    if obs("GetRecordStatus").get("outputActive"):
        say("[STOP] OBS is already recording; not touching it.")
        return 2
    peak = obs.peak_db(args.audio_input, 2)
    if peak > QUIET_DB:
        say(f"[STOP] {args.audio_input} is not quiet (peak {peak:.1f} dB). Pause music/video and run again.")
        return 2
    offset = obs("GetInputAudioSyncOffset", inputName=args.audio_input)["inputAudioSyncOffset"]
    say(f"{args.audio_input}: sync offset {offset} ms")

    server = None
    if not port_open(REFERENCE_PORT):
        server = subprocess.Popen([sys.executable, str(Path(args.reference_dir) / "serve-reference.py"),
                                   "--port", str(REFERENCE_PORT)], cwd=args.reference_dir, creationflags=NO_WINDOW)
        for _ in range(20):
            if port_open(REFERENCE_PORT):
                break
            time.sleep(0.25)
    profile = Path(tempfile.gettempdir()) / "avsync-reference-chrome"
    chrome = subprocess.Popen([find_chrome(), f"--user-data-dir={profile}", f"--remote-debugging-port={DEVTOOLS_PORT}",
                               "--kiosk", "--no-first-run", "--no-default-browser-check", "--window-position=0,0",
                               "--autoplay-policy=no-user-gesture-required", "--disable-background-timer-throttling",
                               "--disable-renderer-backgrounding",
                               f"http://127.0.0.1:{REFERENCE_PORT}/av-reference.html"])
    old_scene = old_dir = recording = None
    status_text = ""
    try:
        tools = None
        for _ in range(60):
            try:
                pages = json.load(urllib.request.urlopen(f"http://127.0.0.1:{DEVTOOLS_PORT}/json", timeout=2))
                page = next(p for p in pages if p.get("type") == "page" and "av-reference" in p.get("url", ""))
                tools = DevTools(page["webSocketDebuggerUrl"])
                break
            except Exception:
                time.sleep(0.5)
        if not tools:
            raise RuntimeError("could not attach to the reference Chrome window")
        for _ in range(40):
            if "Ready." in (tools.evaluate("document.body ? document.body.innerText : ''") or ""):
                break
            time.sleep(0.5)
        else:
            raise RuntimeError("reference page never became Ready")
        time.sleep(3)  # let the window settle before the page measures frame pacing

        old_scene = obs("GetCurrentProgramScene")["currentProgramSceneName"]
        obs("SetCurrentProgramScene", sceneName=args.scene)
        old_dir = obs("GetRecordDirectory")["recordDirectory"]
        subprocess.run(ssh + [f"mkdir -p '{args.remote_dir}'"], timeout=20, creationflags=NO_WINDOW)
        obs("SetRecordDirectory", recordDirectory=args.remote_dir)
        obs("StartRecord")
        for _ in range(40):
            if obs("GetRecordStatus").get("outputActive"):
                break
            time.sleep(0.25)
        else:
            raise RuntimeError("OBS recording did not start")
        time.sleep(1.5)
        say("Recording. Starting the reference pass (about 70 s); don't touch the main monitor.")
        tools.evaluate("document.getElementById('start').click()", gesture=True)
        deadline = time.monotonic() + PASS_TIMEOUT
        while time.monotonic() < deadline:
            status_text = tools.evaluate("document.body.innerText") or ""
            if "Source scheduling completed" in status_text or "Stopped by" in status_text:
                break
            time.sleep(1)
        time.sleep(3)
        recording = obs("StopRecord").get("outputPath")
    finally:
        try:
            if obs("GetRecordStatus").get("outputActive"):
                recording = obs("StopRecord").get("outputPath")
        except Exception:
            pass
        time.sleep(2)
        if old_dir:
            obs("SetRecordDirectory", recordDirectory=old_dir)
        if old_scene:
            obs("SetCurrentProgramScene", sceneName=old_scene)
        if os.name == "nt":
            subprocess.run(["taskkill", "/F", "/T", "/PID", str(chrome.pid)], capture_output=True, creationflags=NO_WINDOW)
        else:
            chrome.kill()
        if server:
            server.kill()

    page_line = next((line for line in status_text.splitlines() if "completed" in line or "Stopped by" in line), "")
    say(f"Page: {page_line}")
    if not recording:
        say("[FAIL] No recording was produced.")
        return 1
    tag = time.strftime("%Y%m%d-%H%M%S")
    say("Analyzing on the OBS machine...")
    remote = (f"cd '{args.remote_dir}' && mv -f '{recording}' 'synctest-{tag}-full.mkv' && "
              f"ffmpeg -hide_banner -loglevel error -y -i 'synctest-{tag}-full.mkv' -map 0 -c copy -t 110 "
              f"'synctest-{tag}.mkv' && cd '{args.remote_repo}' && python3 tools/measure_physical.py "
              f"'{args.remote_dir}/synctest-{tag}.mkv' --audio-track 0 --max-median-ms 16.667 --max-offset-ms 33.333")
    out = subprocess.run(ssh + [remote], capture_output=True, text=True, timeout=300, creationflags=NO_WINDOW)
    try:
        result = json.loads(out.stdout)
    except json.JSONDecodeError:
        result = {"error": (out.stdout + out.stderr).strip()[-800:]}
    with open(args.results, "a", encoding="utf-8") as handle:
        handle.write(json.dumps({"time": time.strftime("%Y-%m-%dT%H:%M:%S"), "sync_offset_ms": offset,
                                 "page_status": page_line, "recording": f"synctest-{tag}.mkv", "result": result}) + "\n")
    if not result.get("valid_marker_match"):
        say(f"[FAIL] Analyzer could not match markers: {result.get('error')}")
        return 1
    events = [e["audio_minus_video_ms"] for e in result["events"]]
    median = result.get("median_audio_minus_video_ms", statistics.median(events))
    say("audio minus video per marker (ms): " + ", ".join(f"{e:+.1f}" for e in events))
    say(f"median: {median:+.1f} ms   spread: {max(events) - min(events):.1f} ms   (positive = audio later)")
    gate = result.get("timing_gate", {})
    say("GATE: PASS" if gate.get("passed") else "GATE: FAIL -> " + "; ".join(gate.get("violations", [])))
    suggested = round(offset - median)
    if abs(median) > 5:
        say(f"Suggested {args.audio_input} offset: {suggested} ms (now {offset} ms).")
        if args.apply:
            obs("SetInputAudioSyncOffset", inputName=args.audio_input, inputAudioSyncOffset=suggested)
            say(f"Applied {suggested} ms. Run the test again to confirm.")
    return 0 if gate.get("passed") else 1


if __name__ == "__main__":
    sys.exit(main())
