# SPDX-License-Identifier: GPL-2.0-or-later
"""Generated state/systemd responses only: never starts a service or receiver."""

from __future__ import annotations

from contextlib import contextmanager, redirect_stderr, redirect_stdout
import copy
import importlib.util
import io
import json
import os
from pathlib import Path
import stat
import sys
import tempfile
import threading
import time
import unittest
from unittest import mock

SPEC = importlib.util.spec_from_file_location("remote_receiver", Path(__file__).resolve().parents[1] / "tools" / "remote_receiver.py")
remote = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(remote)
RUN = "1" * 32
CHALLENGE = "2" * 32
INVOCATION = "3" * 32
SESSION = "18446744073709551615"
BOOT = "12345678-1234-1234-1234-123456789abc"
OTHER_BOOT = "abcdef12-1234-1234-1234-123456789abc"
ARGV = ("/private/receiver", "--expect-sender-session", "{sender_session}", "--seconds", "{seconds}", "--control-stdin")
SESSION_ARGV = tuple("--session-seconds" if arg == "--seconds" else arg for arg in ARGV)


class MemoryStore:
    def __init__(self):
        self.run_id, self.session = RUN, SESSION
        self.data = {}
        self.lock = threading.Lock()
        self.locked_now = False

    @contextmanager
    def locked(self, deadline):
        if not self.lock.acquire(timeout=max(0, deadline - time.monotonic())):
            raise remote.RemoteError("lock_timeout")
        self.locked_now = True
        try:
            yield self
        finally:
            self.locked_now = False
            self.lock.release()

    def read(self, name):
        if not self.locked_now:
            raise AssertionError("state read outside authorization lock")
        return copy.deepcopy(self.data.get(name))

    def create(self, name, value):
        if not self.locked_now or name in self.data:
            raise AssertionError("unsafe state creation")
        self.data[name] = copy.deepcopy(value)


def intent(store=None):
    store = store or MemoryStore()
    remote.prepare_intent(store, ARGV, 15, BOOT, time.monotonic() + 1)
    return store


def receipt(store):
    value = {key: store.data["intent.json"][key] for key in ("schema", "run_id", "sender_session", "boot_id", "unit")}
    value.update(invocation_id=INVOCATION, cgroup="/user.slice/" + remote.unit_name(RUN), cgroup_dev=1, cgroup_ino=2)
    store.data["receipt.json"] = value
    return value


def show(active="active", **changes):
    terminal = active in ("inactive", "failed")
    result = dict.fromkeys(remote.PROPERTIES, "")
    result.update(Id=remote.unit_name(RUN), LoadState="loaded", ActiveState=active,
                  SubState="dead" if terminal else "running", InvocationID=INVOCATION,
                  ControlGroup="/user.slice/" + remote.unit_name(RUN), MainPID="0" if terminal else "42",
                  ControlPID="0", Job="0", Transient="yes", Type="exec", Restart="no",
                  KillMode="control-group", SendSIGKILL="yes", RuntimeMaxUSec="20s", TimeoutStopUSec="2s")
    result.update(changes)
    return result


class Backend:
    def __init__(self, shows=None, groups=None):
        self.shows = list(shows or [show("inactive")])
        self.groups = list(groups or ["empty"])
        self.stops, self.queries = [], 0

    def show(self, unit, deadline):
        self.queries += 1
        value = self.shows.pop(0) if len(self.shows) > 1 else self.shows[0]
        if isinstance(value, Exception):
            raise value
        return value

    def cgroup(self, value):
        result = self.groups.pop(0) if len(self.groups) > 1 else self.groups[0]
        if isinstance(result, Exception):
            raise result
        return result

    def stop(self, unit, deadline):
        self.stops.append(unit)

    def gate_identity(self, value, deadline):
        return {"invocation_id": INVOCATION, "cgroup": "/user.slice/" + remote.unit_name(RUN), "cgroup_dev": 1, "cgroup_ino": 2}


class Executed(Exception):
    pass


class ProtocolTests(unittest.TestCase):
    def retire(self, store, backend=None, boot=BOOT, seconds=1):
        return remote.retire(store, backend or Backend(), boot, CHALLENGE, time.monotonic() + seconds)

    def test_strict_tokens_and_config(self):
        remote.identity(RUN, SESSION)
        for run, session in (("../" + RUN, SESSION), (RUN.upper().replace("1", "A"), SESSION),
                             (RUN, "01"), (RUN, "0"), (RUN, str(1 << 64)), (True, SESSION)):
            with self.subTest(run=run, session=session), self.assertRaises(remote.RemoteError):
                remote.identity(run, session)
        self.assertEqual(remote.validate_argv(list(ARGV)), ARGV)
        for argv in ([], list(ARGV[:-1]), ["receiver", *ARGV[1:]], [*ARGV, "{clock_epoch}"],
                     [*ARGV, "--seconds", "{seconds}"], [*ARGV, "embedded-{seconds}"], [*ARGV, "line\nbreak"]):
            with self.subTest(argv=argv), self.assertRaises(remote.RemoteError):
                remote.validate_argv(argv)

    def test_intent_is_single_use_and_binds_full_argv(self):
        store = intent()
        self.assertEqual(store.data["intent.json"]["receiver_argv"], list(ARGV))
        self.assertEqual(store.data["intent.json"]["boot_id"], BOOT)
        with self.assertRaisesRegex(remote.RemoteError, "already_reserved"):
            remote.prepare_intent(store, ARGV, 15, BOOT, time.monotonic() + 1)

    def test_session_argv_requires_matching_explicit_mode(self):
        self.assertEqual(remote.validate_argv(list(SESSION_ARGV), session_mode=True), SESSION_ARGV)
        for argv, mode in ((SESSION_ARGV, False), (ARGV, True), (SESSION_ARGV, 1),
                           ((*SESSION_ARGV, "--seconds", "{seconds}"), True),
                           ((*SESSION_ARGV, "--seconds=180"), True),
                           ((*SESSION_ARGV, "--session-seconds=43200"), True),
                           ((*SESSION_ARGV, "--session-seconds", "{seconds}"), True),
                           ((*SESSION_ARGV, "--recover-desktop-ipc"), True),
                           ((*SESSION_ARGV, "--clock-pause-after", "5"), True),
                           ((*SESSION_ARGV, "--clock-pause-seconds=2"), True)):
            with self.subTest(argv=argv, mode=mode), self.assertRaises(remote.RemoteError):
                remote.validate_argv(list(argv), session_mode=mode)

    def test_session_intent_preserves_schema_and_authorizes_exact_duration(self):
        store = MemoryStore()
        value = remote.prepare_intent(store, SESSION_ARGV, 43200, BOOT, time.monotonic() + 1, session_mode=True)
        self.assertEqual(set(value), set(intent().data["intent.json"]))
        self.assertEqual(value["seconds"], 43200)
        self.assertEqual(remote.validate_intent(value, RUN, SESSION), value)
        self.assertIn("--property=RuntimeMaxSec=43205", remote.start_argv(Path("/private/state"), value))
        def execute(argv):
            self.assertTrue(store.locked_now)
            self.assertIn("receipt.json", store.data)
            self.assertEqual(argv, ["/private/receiver", "--expect-sender-session", SESSION,
                                    "--session-seconds", "43200", "--control-stdin"])
            raise Executed()
        with self.assertRaises(Executed):
            remote.authorize_gate(store, Backend(), BOOT, time.monotonic() + 1, execute)
        self.assertEqual(self.retire(store)["proof"], "fenced_empty_cgroup")

    def test_session_limits_do_not_expand_legacy_diagnostics(self):
        for argv, seconds, mode in ((ARGV, 181, False), (SESSION_ARGV, 43201, True),
                                    (SESSION_ARGV, 0, True), (SESSION_ARGV, True, True),
                                    (SESSION_ARGV, 181, False), (ARGV, 181, True)):
            store = MemoryStore()
            with self.subTest(argv=argv, seconds=seconds, mode=mode), self.assertRaises(remote.RemoteError):
                remote.prepare_intent(store, argv, seconds, BOOT, time.monotonic() + 1, session_mode=mode)
            self.assertEqual(store.data, {})
        legacy = intent().data["intent.json"]
        self.assertEqual(remote.validate_intent(legacy, RUN, SESSION), legacy)
        for updates in ({"seconds": 181}, {"seconds": True},
                        {"receiver_argv": list(SESSION_ARGV), "seconds": 43201},
                        {"receiver_argv": [*SESSION_ARGV, "--seconds", "{seconds}"]},
                        {"session_mode": True}):
            with self.subTest(updates=updates), self.assertRaises(remote.RemoteError):
                remote.validate_intent({**legacy, **updates}, RUN, SESSION)

    def test_session_gate_requires_matching_finite_runtime_policy(self):
        store = MemoryStore()
        value = remote.prepare_intent(store, SESSION_ARGV, 43200, BOOT, time.monotonic() + 1, session_mode=True)
        backend = remote.Systemd()
        cgroup = "/user.slice/" + remote.unit_name(RUN)
        info = mock.Mock(st_mode=stat.S_IFDIR | 0o700, st_dev=1, st_ino=2)
        for runtime, valid in (("12h 5s", True), ("20s", False), ("infinity", False)):
            state = show(MainPID=str(os.getpid()), RuntimeMaxUSec=runtime)
            with self.subTest(runtime=runtime), mock.patch.dict(os.environ, INVOCATION_ID=INVOCATION), \
                    mock.patch("builtins.open", mock.mock_open(read_data=("0::" + cgroup + "\n").encode())), \
                    mock.patch.object(backend, "show", return_value=state), mock.patch.object(os, "stat", return_value=info):
                if valid:
                    result = backend.gate_identity(value, time.monotonic() + 1)
                    self.assertEqual(result["invocation_id"], INVOCATION)
                    self.assertEqual(result["cgroup"], cgroup)
                else:
                    with self.assertRaises(remote.RemoteError):
                        backend.gate_identity(value, time.monotonic() + 1)

    def test_session_cancellation_still_fences_delayed_gate(self):
        store = MemoryStore()
        remote.prepare_intent(store, SESSION_ARGV, 43200, BOOT, time.monotonic() + 1, session_mode=True)
        self.assertEqual(self.retire(store)["proof"], "fenced_never_started")
        with self.assertRaisesRegex(remote.RemoteError, "cancelled"):
            remote.authorize_gate(store, Backend(), BOOT, time.monotonic() + 1, lambda argv: self.fail("executed"))

    def test_start_cli_selects_mode_without_opening_state(self):
        base = ["remote_receiver.py", "start", "--state-dir", "/private/state", "--run-id", RUN,
                "--expect-sender-session", SESSION, "--config", "/private/receiver.json", "--control-stdin"]
        for flag, seconds, mode in (("--seconds", 180, False), ("--session-seconds", 43200, True)):
            with self.subTest(flag=flag), mock.patch.object(sys, "platform", "linux"), \
                    mock.patch.object(sys, "argv", base + [flag, str(seconds)]), \
                    mock.patch.object(remote, "load_config", return_value=SESSION_ARGV if mode else ARGV) as load, \
                    mock.patch.object(remote, "start", return_value=0) as start:
                self.assertEqual(remote.main(), 0)
                load.assert_called_once_with(Path("/private/receiver.json"), session_mode=mode)
                self.assertEqual(start.call_args.args[2], seconds)
                self.assertEqual(start.call_args.kwargs, {"session_mode": mode})
        for tail in ([], ["--seconds", "180", "--session-seconds", "43200"]):
            with self.subTest(tail=tail), mock.patch.object(sys, "argv", base + tail), \
                    mock.patch.object(remote, "load_config") as load, redirect_stderr(io.StringIO()):
                self.assertEqual(remote.main(), 2)
                load.assert_not_called()

    def test_retire_before_start_irreversibly_reserves_run(self):
        store, backend = MemoryStore(), Backend()
        result = self.retire(store, backend)
        self.assertEqual(result["proof"], "fenced_never_started")
        self.assertEqual(backend.queries, 0)
        self.assertEqual(backend.stops, [])
        with self.assertRaisesRegex(remote.RemoteError, "cancelled"):
            remote.prepare_intent(store, ARGV, 15, BOOT, time.monotonic() + 1)
        with self.assertRaisesRegex(remote.RemoteError, "cancelled"):
            remote.authorize_gate(store, backend, BOOT, time.monotonic() + 1, lambda argv: self.fail("executed"))

    def test_cancelled_delayed_gate_never_executes(self):
        store = intent()
        self.assertEqual(self.retire(store)["proof"], "fenced_never_started")
        with self.assertRaisesRegex(remote.RemoteError, "cancelled"):
            remote.authorize_gate(store, Backend(), BOOT, time.monotonic() + 1, lambda argv: self.fail("executed"))

    def test_receipt_precedes_exec_under_same_lock(self):
        store = intent()
        def execute(argv):
            self.assertTrue(store.locked_now)
            self.assertEqual(store.data["receipt.json"]["invocation_id"], INVOCATION)
            self.assertEqual(argv, ["/private/receiver", "--expect-sender-session", SESSION, "--seconds", "15", "--control-stdin"])
            raise Executed()
        with self.assertRaises(Executed):
            remote.authorize_gate(store, Backend(), BOOT, time.monotonic() + 1, execute)
        with self.assertRaisesRegex(remote.RemoteError, "reused"):
            remote.authorize_gate(store, Backend(), BOOT, time.monotonic() + 1, execute)

    def test_gate_on_wrong_boot_cannot_execute(self):
        store = intent()
        with self.assertRaisesRegex(remote.RemoteError, "reused"):
            remote.authorize_gate(store, Backend(), OTHER_BOOT, time.monotonic() + 1, lambda argv: self.fail("executed"))

    def test_retire_waits_for_inflight_gate_lock(self):
        store = intent()
        store.lock.acquire()
        try:
            with self.assertRaisesRegex(remote.RemoteError, "lock_timeout"):
                self.retire(store, seconds=.02)
        finally:
            store.lock.release()
        self.assertNotIn("cancel.json", store.data)

    def test_cooperative_terminal_and_empty_group_prove_retirement(self):
        store = intent()
        receipt(store)
        backend = Backend()
        result = self.retire(store, backend)
        self.assertEqual(result, {"schema": 1, "event": "receiver_retired", "run_id": RUN,
                                 "sender_session": SESSION, "challenge": CHALLENGE, "proof": "fenced_empty_cgroup"})
        self.assertEqual(backend.stops, [])
        self.assertLess(len("AVSYNC_RETIRE " + json.dumps(result)), 2048)

    def test_active_unit_stopped_only_by_exact_unit_identity(self):
        store = intent()
        receipt(store)
        backend = Backend([show(), show("inactive")], ["populated", "empty"])
        self.assertEqual(self.retire(store, backend)["proof"], "fenced_empty_cgroup")
        self.assertEqual(backend.stops, [remote.unit_name(RUN)])

    def test_systemd255_failed_empty_job_and_cgroup_after_stop(self):
        store = intent()
        receipt(store)
        recorded = show("failed", SubState="failed", ControlGroup="", Job="")
        # All requested properties, including the explicitly empty Job, came
        # from a complete systemctl show response, not a missing-field default.
        data = "\n".join(key + "=" + value for key, value in recorded.items()).encode()
        parsed = remote.parse_show(0, data, remote.unit_name(RUN))
        self.assertEqual(self.retire(store, Backend([parsed], ["absent"]))["proof"], "fenced_empty_cgroup")
        for job in ("7 /org/freedesktop/systemd1/job/7", "7", " "):
            with self.subTest(job=job), self.assertRaises(remote.RemoteError):
                self.retire(store, Backend([{**parsed, "Job": job}], ["absent"]))
        incomplete = {key: value for key, value in parsed.items() if key != "Job"}
        data = "\n".join(key + "=" + value for key, value in incomplete.items()).encode()
        with self.assertRaises(remote.RemoteError):
            remote.parse_show(0, data, remote.unit_name(RUN))

    def test_gc_requires_recorded_group_empty_or_absent(self):
        for group in ("empty", "absent"):
            store = intent()
            receipt(store)
            backend = Backend([show(LoadState="not-found")], [group])
            self.assertEqual(self.retire(store, backend)["proof"], "fenced_empty_cgroup")
        store = intent()
        receipt(store)
        with self.assertRaisesRegex(remote.RemoteError, "still_populated"):
            self.retire(store, Backend([show(LoadState="not-found")], ["populated"]))

    def test_terminal_unit_is_not_proof_with_live_descendants(self):
        store = intent()
        receipt(store)
        backend = Backend(groups=["populated"])
        with self.assertRaisesRegex(remote.RemoteError, "retirement_timeout"):
            self.retire(store, backend, seconds=.07)
        self.assertEqual(backend.stops, [remote.unit_name(RUN)])

    def test_empty_group_with_active_or_pending_unit_is_not_proof(self):
        for state in (show(), show("inactive", Job="7 /org/freedesktop/systemd1/job/7"),
                      show("inactive", ControlPID="42")):
            store = intent()
            receipt(store)
            with self.assertRaisesRegex(remote.RemoteError, "retirement_timeout"):
                self.retire(store, Backend([state], ["empty"]), seconds=.02)

    def test_mismatched_identity_never_stops_unit(self):
        for fields in ({"InvocationID": "4" * 32}, {"Id": "other.service"}, {"ControlGroup": "/other"},
                       {"Restart": "always"}, {"Transient": "no"}):
            store = intent()
            receipt(store)
            backend = Backend([show(**fields)])
            with self.assertRaisesRegex(remote.RemoteError, "identity_mismatch"):
                self.retire(store, backend)
            self.assertEqual(backend.stops, [])
            self.assertIn("cancel.json", store.data)

    def test_unavailable_manager_or_changed_cgroup_stays_unknown(self):
        for backend in (Backend([remote.RemoteError("unit_query_failed")]),
                        Backend(groups=[remote.RemoteError("cgroup_identity_changed")])):
            store = intent()
            receipt(store)
            with self.assertRaises(remote.RemoteError):
                self.retire(store, backend)
            self.assertIn("cancel.json", store.data)

    def test_prior_boot_is_fenced_without_signalling_current_unit(self):
        store = intent()
        receipt(store)
        backend = Backend()
        self.assertEqual(self.retire(store, backend, boot=OTHER_BOOT)["proof"], "fenced_prior_boot")
        self.assertEqual(backend.queries, 0)
        self.assertEqual(backend.stops, [])

    def test_wrong_session_and_corrupt_state_fail_closed(self):
        for name in ("intent.json", "receipt.json", "cancel.json"):
            store = intent()
            receipt(store)
            store.data["cancel.json"] = {"schema": 1, "run_id": RUN, "sender_session": SESSION}
            store.data[name]["sender_session"] = "7"
            with self.subTest(name=name), self.assertRaises(remote.RemoteError):
                self.retire(store)
        store = MemoryStore()
        store.data["receipt.json"] = {"schema": 1}
        with self.assertRaisesRegex(remote.RemoteError, "without_intent"):
            self.retire(store)

    def test_receipt_rejects_unbounded_or_foreign_paths(self):
        for fields in ({"cgroup": "/"}, {"cgroup": "/../" + remote.unit_name(RUN)}, {"cgroup_ino": True},
                       {"cgroup_dev": 0}, {"invocation_id": "0" * 32}, {"boot_id": OTHER_BOOT}):
            store = intent()
            value = receipt(store)
            value.update(fields)
            with self.subTest(fields=fields), self.assertRaises(remote.RemoteError):
                self.retire(store)

    def test_query_schema_does_not_promote_arbitrary_error_to_not_found(self):
        valid = show()
        data = "\n".join(key + "=" + value for key, value in valid.items()).encode()
        self.assertEqual(remote.parse_show(0, data, remote.unit_name(RUN)), valid)
        missing = ("Id=" + remote.unit_name(RUN) + "\nLoadState=not-found\n").encode()
        self.assertEqual(remote.parse_show(4, missing, remote.unit_name(RUN))["LoadState"], "not-found")
        for code, raw in ((1, b""), (1, data), (0, b"LoadState=not-found\n"), (0, data + b"\nId=other"),
                          (0, b"\xff"), (0, data + b"\nPrivateError=secret")):
            with self.subTest(code=code, raw=raw[:20]), self.assertRaises(remote.RemoteError):
                remote.parse_show(code, raw, remote.unit_name(RUN))

    def test_systemd_launch_is_finite_and_does_not_install_startup(self):
        args = remote.start_argv(Path("/private/state"), intent().data["intent.json"])
        for value in ("--quiet", "--pipe", "--wait", "--property=Type=exec", "--property=Restart=no",
                      "--property=RuntimeMaxSec=20", "--property=TimeoutStopSec=2",
                      "--property=KillMode=control-group", "--property=SendSIGKILL=yes"):
            self.assertIn(value, args)
        self.assertNotIn("--scope", args)
        self.assertNotIn("--collect", args)
        self.assertNotIn("enable", args)
        self.assertEqual(remote.duration_us("3min 5s"), 185_000_000)
        self.assertEqual(remote.duration_us("2000ms"), 2_000_000)
        for value in ("infinity", "-2s", "2", "2s;private", ""):
            with self.assertRaises(remote.RemoteError):
                remote.duration_us(value)

    def test_errors_do_not_publish_private_arguments(self):
        stderr, stdout = io.StringIO(), io.StringIO()
        with mock.patch.object(sys, "argv", ["remote_receiver.py", "start", "--private-secret"]), redirect_stderr(stderr), redirect_stdout(stdout):
            code = remote.main()
        self.assertNotEqual(code, 0)
        self.assertEqual(stdout.getvalue(), "")
        self.assertNotIn("private-secret", stderr.getvalue())
        self.assertEqual(json.loads(stderr.getvalue())["reason"], "invalid_arguments")


@unittest.skipUnless(sys.platform == "linux", "Linux private state filesystem checks")
class FilesystemTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.path = Path(self.temp.name)
        self.path.chmod(0o700)
        self.store = remote.Attempt(self.path, RUN, SESSION)

    def test_cancel_durable_and_single_use(self):
        result = remote.retire(self.store, Backend(), BOOT, CHALLENGE, time.monotonic() + 1)
        self.assertEqual(result["proof"], "fenced_never_started")
        self.assertEqual(stat.S_IMODE((self.path / RUN / "cancel.json").stat().st_mode), 0o600)
        other = remote.Attempt(self.path, RUN, SESSION)
        with self.assertRaisesRegex(remote.RemoteError, "cancelled"):
            remote.prepare_intent(other, ARGV, 15, BOOT, time.monotonic() + 1)

    def test_lock_does_not_survive_exec_and_is_not_replaced(self):
        with self.store.locked(time.monotonic() + 1):
            first = (self.path / RUN / "lock").stat().st_ino
            self.assertFalse(os.get_inheritable(self.store.fd))
        with self.store.locked(time.monotonic() + 1):
            self.assertEqual(first, (self.path / RUN / "lock").stat().st_ino)

    def test_symlink_lock_and_state_are_rejected(self):
        directory = self.path / RUN
        directory.mkdir(mode=0o700)
        target = self.path / "target"
        target.write_text("{}")
        target.chmod(0o600)
        (directory / "lock").symlink_to(target)
        with self.assertRaises(OSError):
            with self.store.locked(time.monotonic() + 1):
                self.fail("unsafe lock opened")
        (directory / "lock").unlink()
        (directory / "intent.json").symlink_to(target)
        with self.assertRaises(OSError):
            remote.cancellation_snapshot(self.store, time.monotonic() + 1)

    def test_unsafe_parent_and_hardlinked_state_are_rejected(self):
        self.path.chmod(0o755)
        with self.assertRaises(remote.RemoteError):
            with self.store.locked(time.monotonic() + 1):
                self.fail("unsafe parent opened")
        self.path.chmod(0o700)
        with self.store.locked(time.monotonic() + 1):
            self.store.create("intent.json", {"private": "value"})
        os.link(self.path / RUN / "intent.json", self.path / "hardlink")
        with self.assertRaises(remote.RemoteError):
            remote.cancellation_snapshot(self.store, time.monotonic() + 1)

    def test_no_writer_fifo_config_and_record_do_not_block(self):
        fifo = self.path / "config-fifo"
        os.mkfifo(fifo, mode=0o600)
        started = time.monotonic()
        with self.assertRaises(remote.RemoteError):
            remote.load_config(fifo)
        with self.store.locked(time.monotonic() + 1):
            os.mkfifo("intent.json", mode=0o600, dir_fd=self.store.fd)
            with self.assertRaises(remote.RemoteError):
                self.store.read("intent.json")
        self.assertLess(time.monotonic() - started, 1)


if __name__ == "__main__":
    unittest.main()
