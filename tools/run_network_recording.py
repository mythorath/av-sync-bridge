#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Explicit Linux-loopback generated network audio recording, not physical A/V.

Uses a private X server, isolated libOBS, and finite transient receiver services.
No normal OBS profile, physical capture, microphone, playback, or remote host is
opened. A restart proves fresh audio after retirement, NOT queued-stale rejection.
The same trusted local helper and private state directory start and retire every
receiver. Never point these at a different control domain.
"""
from __future__ import annotations

import argparse
from dataclasses import dataclass, field
import json
import os
from pathlib import Path
import secrets
import signal
import subprocess
import sys
import threading
import time

from measure_network_recording import parse_manifest
from process_pair import parse_control, parse_native_summary
from retirement_proof import parse_retirement
from run_restart_recording import wait_display_fd

MAX_LOG = 1024 * 1024
CONTROL_SECONDS = 115
DECODE_SECONDS = 100
SCOPE = "generated Linux loopback network desktop audio only; NOT physical A/V calibration"


class TrialFailure(Exception):
    """Only fixed, non-sensitive codes may enter the public report."""


def unique_object(pairs):
    value = {}
    for key, item in pairs:
        if key in value:
            raise TrialFailure("duplicate_json_key")
        value[key] = item
    return value


def decode_json(text):
    def reject(_):
        raise TrialFailure("nonfinite_json")
    try:
        return json.loads(text, object_pairs_hook=unique_object, parse_constant=reject)
    except (ValueError, UnicodeError, RecursionError):
        raise TrialFailure("invalid_json") from None


def read_bytes(path: Path) -> bytes:
    with path.open("rb") as handle:
        data = handle.read(MAX_LOG + 1)
    if len(data) > MAX_LOG:
        raise TrialFailure("child_output_limit")
    return data


def decode_text(data: bytes) -> str:
    try:
        return data.decode("utf-8")
    except UnicodeError:
        raise TrialFailure("child_output_encoding") from None


def read_log(path: Path) -> str:
    return decode_text(read_bytes(path))


def complete_lines(path: Path):
    # A concurrently written final line is not a complete protocol record yet.
    data = read_bytes(path)
    complete = data[:data.rfind(b"\n") + 1]
    return decode_text(complete).split("\n")[:-1]


@dataclass(eq=False)
class Child:
    process: subprocess.Popen
    path: Path
    reader: threading.Thread | None = None
    errors: list[str] = field(default_factory=list)

    def poll(self):
        return self.process.poll()


def drain_output(pipe, handle, errors: list[str]) -> None:
    """At most MAX_LOG bytes ever reach disk; no unbounded communicate buffer."""
    count = 0
    try:
        while chunk := pipe.read(4096):
            if count + len(chunk) > MAX_LOG:
                errors.append("child_output_limit")
                break
            if handle.write(chunk) != len(chunk):
                raise OSError("short_log_write")
            count += len(chunk)
    except (OSError, ValueError):
        errors.append("child_output_read_error")
    finally:
        for stream in (handle, pipe):
            try:
                stream.close()
            except OSError:
                if not errors:
                    errors.append("child_output_close_error")


def stop_owned(child: Child) -> None:
    """Signal only a still-owned session leader and its own helper group."""
    process = child.process
    if process.poll() is None:
        try:
            if os.getpgid(process.pid) != process.pid:
                raise TrialFailure("owned_session_mismatch")
            os.killpg(process.pid, signal.SIGCONT)
            os.killpg(process.pid, signal.SIGTERM)
            try:
                process.wait(timeout=1)
            except subprocess.TimeoutExpired:
                os.killpg(process.pid, signal.SIGKILL)
                process.wait(timeout=1)
        except ProcessLookupError:
            process.wait(timeout=1)
    if process.stdin:
        process.stdin.close()
    if child.reader:
        child.reader.join(timeout=.5)
        if child.reader.is_alive():
            raise TrialFailure("child_output_not_closed")


def receiver_config(args, ipc: Path) -> dict:
    return {"receiver_argv": [str(args.network_build_dir / "avsync-network-receiver"),
        "--bind", "127.0.0.1", "--peer", "127.0.0.1", "--clock-port", str(args.clock_port),
        "--rtp-port", str(args.rtp_port), "--rtcp-port", str(args.rtcp_port),
        "--desktop-ipc", str(ipc), "--replace-desktop-ipc", "--expect-sender-session",
        "{sender_session}", "--seconds", "{seconds}", "--control-stdin"]}


class Trial:
    def __init__(self, args):
        self.args = args
        self.root = args.output_dir
        self.state = self.root / "state"
        self.state.mkdir(mode=0o700)
        self.helper = [sys.executable, str(args.receiver_helper)]
        self.deadline = time.monotonic() + CONTROL_SECONDS
        self.children: list[Child] = []
        self.controlled: dict[Child, float] = {}
        self.pending: list[tuple[str, str]] = []
        self.retire_queries = 0
        self.reports = []
        self.display = self.obs = None

    def launch(self, argv, name, *, control=False, env=None, pass_fds=()) -> Child:
        path = self.root / name
        handle = path.open("xb", buffering=0)
        try:
            process = subprocess.Popen(argv, stdin=subprocess.PIPE if control else subprocess.DEVNULL,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, pass_fds=pass_fds,
                start_new_session=True, shell=False, bufsize=0)
        except BaseException:
            handle.close()
            raise
        child = Child(process, path)
        self.children.append(child)  # Own it before any further operation can fail.
        child.reader = threading.Thread(target=drain_output,
            args=(process.stdout, handle, child.errors), daemon=True)
        child.reader.start()
        if control:
            os.set_blocking(process.stdin.fileno(), False)
            self.controlled[child] = 0
        return child

    @staticmethod
    def send(child: Child, value: bytes):
        try:
            count = os.write(child.process.stdin.fileno(), value)
        except (OSError, ValueError):
            raise TrialFailure("control_write_failed") from None
        if count != len(value):
            raise TrialFailure("short_control_write")

    def check_output(self):
        if any(child.errors for child in self.children):
            raise TrialFailure("child_output_failed")

    def pump(self):
        now = time.monotonic()
        if now >= self.deadline:
            raise TrialFailure("whole_trial_deadline")
        self.check_output()
        for child, last in list(self.controlled.items()):
            if child.poll() is not None:
                raise TrialFailure("controlled_child_exited")
            if now - last >= .5:
                self.send(child, b"AVSYNC_KEEPALIVE\n")
                self.controlled[child] = now
        for child in (self.display, self.obs):
            if child and child.poll() is not None:
                raise TrialFailure("recorder_exited_early")

    def wait_record(self, child, prefix, match, seconds):
        until = min(self.deadline, time.monotonic() + seconds)
        while time.monotonic() < until:
            self.pump()
            records = []
            for line in complete_lines(child.path):
                if line.startswith(prefix):
                    if len(line) > 2048:
                        raise TrialFailure("protocol_record_limit")
                    value = decode_json(line[len(prefix):])
                    if not isinstance(value, dict):
                        raise TrialFailure("invalid_protocol_record")
                    if match(value):
                        records.append(value)
            if len(records) > 1:
                raise TrialFailure("duplicate_protocol_record")
            if child.poll() is not None:
                raise TrialFailure("child_exited_before_record")
            if records:
                return records[0]
            time.sleep(.02)
        raise TrialFailure("fixture_record_timeout")

    def finish_output(self, child):
        if child.reader:
            child.reader.join(timeout=.5)
            if child.reader.is_alive():
                raise TrialFailure("child_output_not_closed")
        self.check_output()

    def retire(self, run_id, session):
        # This intentionally has a fresh bounded cleanup budget, even if the
        # recording deadline expired. Expiry never excuses skipping the fence.
        challenge = secrets.token_hex(16)
        self.retire_queries += 1
        child = self.launch(self.helper + ["retire", "--state-dir", str(self.state), "--run-id", run_id,
            "--expect-sender-session", session, "--challenge", challenge],
            f"retirement-{self.retire_queries}.log")
        failure = None
        proof = None
        try:
            if child.process.wait(timeout=10) != 0:
                raise TrialFailure("retirement_query_failed")
            self.finish_output(child)
            proof = parse_retirement(read_log(child.path).encode("ascii"), run_id, session, challenge)
        except (ValueError, UnicodeError, subprocess.TimeoutExpired):
            failure = TrialFailure("retirement_not_verified")
        except Exception as error:
            failure = error
        finally:
            try:
                stop_owned(child)
            except Exception:
                if failure is None:
                    failure = TrialFailure("retirement_local_cleanup_failed")
        if failure is not None:
            raise failure
        return {"verified": True, "proof": proof}

    def native_receiver(self, child):
        lines = [line for line in complete_lines(child.path) if line.startswith("{")]
        if len(lines) != 1:
            raise TrialFailure("native_receiver_summary_missing")
        report = parse_native_summary(lines[0].encode(), "receiver")
        if not report["reported_media_qualified"]:
            (self.root / "unqualified-receiver.json").write_text(json.dumps(report), encoding="utf-8")
            raise TrialFailure("native_receiver_not_qualified")
        return report

    def native_sender(self, child, count):
        lines = [line for line in complete_lines(child.path) if line.startswith("{")]
        if len(lines) != 1:
            raise TrialFailure("native_sender_summary_missing")
        value = decode_json(lines[0])
        positive = ("generated_packets", "generated_frames", "mapped_packets", "rtp_packets_at_output",
                    "anchored_rtp_packets", "sender_reports")
        zero = ("anchor_transport_errors", "queue_overflows", "clock_loss_count")
        if (not isinstance(value, dict) or type(value.get("schema")) is not int or value["schema"] != 1 or
                value.get("status") != "control_stopped" or value.get("generated_audio") is not True or
                value.get("reason") != "none" or value.get("clock_usable") is not True or
                value.get("original_anchors_transmitted") is not True or value.get("media_verified") is not False or
                type(value.get("generated_markers")) is not int or value["generated_markers"] != count or
                any(type(value.get(k)) is not int or not 0 < value[k] < 2**64 for k in positive) or
                any(type(value.get(k)) is not int or value[k] != 0 for k in zero) or
                value["rtp_packets_at_output"] != value["anchored_rtp_packets"] or
                value["mapped_packets"] != value["generated_packets"] or
                value["generated_frames"] != 480 * value["generated_packets"]):
            raise TrialFailure("native_sender_not_qualified")
        return {k: value[k] for k in (*positive, *zero, "generated_markers")}

    @staticmethod
    def manifest(child, role, count, session, epoch):
        try:
            result = parse_manifest(read_log(child.path), role, count)
        except Exception:
            raise TrialFailure("invalid_fixture_manifest") from None
        header = result["header"]
        if header["sender_session"] != session or header["clock_epoch"] != epoch or header["generation"] != "1":
            raise TrialFailure("fixture_identity_mismatch")
        return result

    def setup(self):
        read_fd, write_fd = os.pipe()
        try:
            self.display = self.launch(["Xvfb", "-displayfd", str(write_fd), "-screen", "0", "800x600x24",
                "-nolisten", "tcp", "-noreset"], "xvfb.log", pass_fds=(write_fd,))
            os.close(write_fd)
            write_fd = None
            display = wait_display_fd(read_fd, self.display.process, 10)
        finally:
            os.close(read_fd)
            if write_fd is not None:
                os.close(write_fd)
        env = dict(os.environ, DISPLAY=display)
        env.pop("XAUTHORITY", None)
        self.ipc = self.root / "desktop.ipc"
        self.recording = self.root / "encoded.mkv"
        duration = "45" if self.args.case == "baseline" else "75"
        # A deliberately absent video mapping encodes black without any device.
        self.obs = self.launch([str(self.args.obs_build_dir / "avsync-obs-smoke"),
            str(self.root / "unused-video.ipc"), str(self.recording),
            str(self.args.obs_build_dir / "plugins/obs/avsync-obs.so"), str(self.args.obs_plugins),
            str(self.args.obs_data), duration, "--desktop-ipc-path", str(self.ipc),
            "--audio-lead-ms", "40", "--warmup-seconds", "1", "--scenario", "baseline"], "obs.log", env=env)
        until = min(self.deadline, time.monotonic() + 15)
        while time.monotonic() < until:
            self.pump()
            if sum(line.startswith("AVSYNC_RECORDING_STARTED ") for line in complete_lines(self.obs.path)) == 1:
                break
            time.sleep(.02)
        else:
            raise TrialFailure("recorder_not_started")
        self.config = self.root / "receiver.json"
        self.config.write_text(json.dumps(receiver_config(self.args, self.ipc)), encoding="utf-8")

    def run_roles(self):
        previous_epoch = None
        identities = set()
        for role in ((1,) if self.args.case == "baseline" else (1, 2)):
            self.pump()
            run_id, session = secrets.token_hex(16), str(secrets.randbits(64) or 1)
            if run_id in identities or session in identities:
                raise TrialFailure("generated_identity_collision")
            identities.update((run_id, session))
            self.pending.append((run_id, session))  # Even a failed launch must be fenced.
            receiver = self.launch(self.helper + ["start", "--state-dir", str(self.state), "--config", str(self.config),
                "--run-id", run_id, "--expect-sender-session", session, "--seconds", "58", "--control-stdin"],
                f"receiver-{role}.log", control=True)
            ready = self.wait_record(receiver, "AVSYNC_CONTROL ", lambda v: v.get("event") == "receiver_ready", 8)
            try:
                parse_control(b"AVSYNC_CONTROL " + json.dumps(ready).encode())
            except Exception:
                raise TrialFailure("invalid_receiver_ready") from None
            if ready["sender_session"] != session or ready["clock_epoch"] == previous_epoch:
                raise TrialFailure("receiver_identity_mismatch")
            epoch = ready["clock_epoch"]
            sender = self.launch([str(self.args.network_build_dir / "avsync-network-fixture-sender"),
                "--generated-audio", "--host", "127.0.0.1", "--clock-port", str(self.args.clock_port),
                "--rtp-port", str(self.args.rtp_port), "--rtcp-port", str(self.args.rtcp_port),
                "--clock-epoch", epoch, "--sender-session", session, "--seconds", "55",
                "--fixture-id", str(role), "--control-stdin"], f"sender-{role}.log", control=True)
            ack = self.wait_record(sender, "AVSYNC_CONTROL ", lambda v: v.get("event") == "sender_started", 8)
            if ack != dict(ready, event="sender_started"):
                raise TrialFailure("sender_ack_mismatch")
            count = 3 if self.args.case == "restart" and role == 1 else 6
            marker = self.wait_record(sender, "AVSYNC_FIXTURE ",
                lambda v: v.get("type") == "marker" and v.get("event") == count, 35)
            self.manifest(sender, role, count, session, epoch)
            until_ns = int(marker["capture_ns"]) + 2_200_000_000
            remaining = until_ns - time.monotonic_ns()
            if not 0 <= remaining <= 3_000_000_000:
                raise TrialFailure("unexpected_fixture_clock_domain")
            while time.monotonic_ns() < until_ns:
                self.pump()
                time.sleep(.01)
            interrupted = self.args.case == "restart" and role == 1
            self.controlled.pop(receiver)
            self.controlled.pop(sender)
            if interrupted:
                receiver.process.kill()  # Exact launcher; guardian owns the native receiver.
                receiver.process.wait(timeout=3)
            else:
                self.send(receiver, b"AVSYNC_STOP\n")
            self.send(sender, b"AVSYNC_STOP\n")
            sender_exit = sender.process.wait(timeout=3)
            if not interrupted:
                receiver.process.wait(timeout=3)
            proof = self.retire(run_id, session)
            self.pending.remove((run_id, session))
            self.finish_output(receiver)
            self.finish_output(sender)
            if sender_exit or (not interrupted and receiver.process.returncode):
                raise TrialFailure("nonzero_clean_stop")
            self.manifest(sender, role, count, session, epoch)
            self.reports.append({"role": role, "interrupted": interrupted, "retirement": proof,
                "sender_native": self.native_sender(sender, count),
                "receiver_native": None if interrupted else self.native_receiver(receiver)})
            previous_epoch = epoch

    def record(self):
        self.setup()
        self.run_roles()
        while self.obs.poll() is None:
            if time.monotonic() >= self.deadline:
                raise TrialFailure("whole_trial_deadline")
            self.check_output()
            if self.display.poll() is not None:
                raise TrialFailure("private_display_exited")
            time.sleep(.02)
        self.finish_output(self.obs)
        if self.obs.process.returncode:
            raise TrialFailure("recorder_failed")

    def analyze(self):
        command = [sys.executable, str(Path(__file__).with_name("measure_network_recording.py")),
            "--recording", str(self.recording), "--predecessor-log", str(self.root / "sender-1.log"),
            "--mixer-trace", str(self.recording) + ".mix0.csv"]
        if self.args.case == "restart":
            command += ["--successor-log", str(self.root / "sender-2.log")]
        child = self.launch(command, "analysis.log")
        try:
            code = child.process.wait(timeout=DECODE_SECONDS)
            self.finish_output(child)
            report = decode_json(read_log(child.path))
        except subprocess.TimeoutExpired:
            raise TrialFailure("analysis_deadline") from None
        if not isinstance(report, dict) or code or report.get("passed") is not True:
            raise TrialFailure("measurement_failed")
        # Analyzer is a trusted sibling with its own sanitized output contract.
        return report

    def cleanup(self):
        failures = []
        self.controlled.clear()  # No lease can be renewed after cleanup starts.
        for child in reversed(self.children):
            try:
                stop_owned(child)
                if child.errors:
                    failures.append("owned_child_output_failed")
            except Exception:
                failures.append("owned_child_cleanup_failed")
        # A launcher being gone is never proof that its receiver is gone.
        for run_id, session in list(self.pending):
            try:
                self.retire(run_id, session)
                self.pending.remove((run_id, session))
            except Exception:
                failures.append("retirement_cleanup_failed")
        return sorted(set(failures))


def validate_args(args):
    if sys.platform != "linux" or not args.run_loopback:
        raise TrialFailure("explicit_linux_loopback_required")
    ports = (args.clock_port, args.rtp_port, args.rtcp_port)
    if any(type(p) is not int or not 1024 <= p <= 65535 for p in ports) or len(set(ports)) != 3:
        raise TrialFailure("invalid_loopback_ports")
    for name in ("network_build_dir", "obs_build_dir", "obs_plugins", "obs_data"):
        value = getattr(args, name).resolve(strict=True)
        if not value.is_dir():
            raise TrialFailure("missing_input_directory")
        setattr(args, name, value)
    args.receiver_helper = args.receiver_helper.resolve(strict=True)
    files = (args.network_build_dir / "avsync-network-receiver",
             args.network_build_dir / "avsync-network-fixture-sender",
             args.obs_build_dir / "avsync-obs-smoke", args.obs_build_dir / "plugins/obs/avsync-obs.so",
             args.receiver_helper, Path(__file__).with_name("measure_network_recording.py"))
    if any(not path.is_file() for path in files):
        raise TrialFailure("missing_input_file")
    args.output_dir = args.output_dir.absolute()
    if args.output_dir.exists() or args.output_dir.is_symlink():
        raise TrialFailure("output_directory_exists")
    if not args.output_dir.parent.is_dir():
        raise TrialFailure("output_parent_missing")


def run(args):
    # Refuse an existing directory before spawning or writing anything.
    args.output_dir.mkdir(mode=0o700)
    trial = None
    result = {"case": args.case, "scope": SCOPE, "passed": False,
              "queued_stale_replay_verified": False, "roles": []}
    try:
        trial = Trial(args)
        trial.record()
        result.update(measurement=trial.analyze(), roles=trial.reports, passed=True)
    except Exception as error:
        # The first failure remains authoritative if cleanup also fails.
        result["failure"] = str(error) if isinstance(error, TrialFailure) else "trial_failed"
        try:
            (args.output_dir / "failure.txt").write_text(str(error), encoding="utf-8")
        except OSError:
            result["private_diagnostic_write_failed"] = True
    finally:
        if trial:
            result["roles"] = trial.reports
            try:
                cleanup = trial.cleanup()
            except Exception:
                cleanup = ["cleanup_failed"]
            if cleanup:
                result.update(passed=False, cleanup_failures=cleanup)
    try:
        (args.output_dir / "result.json").write_text(json.dumps(result, indent=2, allow_nan=False) + "\n", encoding="utf-8")
    except OSError:
        result.update(passed=False, report_write_failed=True)
    return result


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-loopback", action="store_true")
    for name in ("network-build-dir", "obs-build-dir", "obs-plugins", "obs-data", "output-dir"):
        parser.add_argument("--" + name, type=Path, required=True)
    parser.add_argument("--receiver-helper", type=Path, default=Path(__file__).with_name("remote_receiver.py"))
    for name in ("clock-port", "rtp-port", "rtcp-port"):
        parser.add_argument("--" + name, type=int, required=True)
    parser.add_argument("--case", choices=("baseline", "restart"), required=True)
    args = parser.parse_args(argv)
    try:
        validate_args(args)
        os.umask(0o077)
        result = run(args)
    except Exception as error:
        result = {"case": args.case, "scope": SCOPE, "passed": False,
                  "failure": str(error) if isinstance(error, TrialFailure) else "setup_failed"}
    print(json.dumps(result, allow_nan=False), flush=True)
    return 0 if result["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
