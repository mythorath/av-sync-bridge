# SPDX-License-Identifier: GPL-2.0-or-later
"""Bounded independent retirement query. Identity agreement is not media proof."""
from __future__ import annotations

import json
import math
import os
import re
import subprocess
import threading
import time

PREFIX = b"AVSYNC_RETIRE "
MAX_PROOF = 2048
PROOFS = {"fenced_empty_cgroup", "fenced_never_started", "fenced_prior_boot"}


def _unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("retirement_duplicate_key")
        result[key] = value
    return result


def parse_retirement(line: bytes, run_id: str, session: str, challenge: str) -> str:
    if (not all(isinstance(value, str) for value in (run_id, session, challenge)) or
            not re.fullmatch(r"[0-9a-f]{32}", run_id) or
            not re.fullmatch(r"[0-9a-f]{32}", challenge) or
            not re.fullmatch(r"[1-9][0-9]{0,19}", session) or int(session) >= 2**64):
        raise ValueError("invalid_retirement_identity")
    if (not isinstance(line, bytes) or len(line) > MAX_PROOF or
            not line.startswith(PREFIX) or not line.endswith(b"\n") or line.count(b"\n") != 1):
        raise ValueError("invalid_retirement_line")
    try:
        message = json.loads(line[len(PREFIX):].decode("ascii"), object_pairs_hook=_unique)
    except (UnicodeError, ValueError, RecursionError):
        raise ValueError("invalid_retirement_json") from None
    if (not isinstance(message, dict) or set(message) !=
            {"schema", "event", "run_id", "sender_session", "challenge", "proof"} or
            type(message["schema"]) is not int or message["schema"] != 1 or
            message["event"] != "receiver_retired" or message["run_id"] != run_id or
            message["sender_session"] != session or message["challenge"] != challenge or
            not isinstance(message["proof"], str) or message["proof"] not in PROOFS):
        raise ValueError("invalid_retirement_proof")
    return message["proof"]


def verify_retirement(argv: list[str], run_id: str, session: str,
                      challenge: str, timeout: float) -> dict:
    answer = {"verified": False, "proof": None, "reason": "retirement_query_failed",
              "forced_local_kill": False}
    if (type(timeout) not in (int, float) or not math.isfinite(timeout) or timeout <= 0 or timeout > 10):
        answer["reason"] = "retirement_budget_exhausted"
        return answer
    deadline = time.monotonic() + timeout
    options = {"creationflags": subprocess.CREATE_NO_WINDOW} if os.name == "nt" else {}
    try:
        child = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, shell=False, bufsize=0, **options)
    except OSError:
        answer["reason"] = "retirement_spawn_failed"
        return answer
    output, failure = bytearray(), threading.Event()
    done = [threading.Event(), threading.Event()]

    def drain(pipe, index, limit):
        count = 0
        try:
            while chunk := pipe.read(4096):
                count += len(chunk)
                if count > limit:
                    failure.set()
                    return
                if index == 0:
                    output.extend(chunk)
        except (OSError, ValueError):
            failure.set()
        finally:
            done[index].set()

    readers = [threading.Thread(target=drain, args=(child.stdout, 0, 4096), daemon=True),
               threading.Thread(target=drain, args=(child.stderr, 1, 65536), daemon=True)]
    for reader in readers:
        reader.start()
    try:
        while time.monotonic() < deadline:
            if failure.is_set():
                answer["reason"] = "retirement_output_error"
                break
            if child.poll() is not None and all(event.is_set() for event in done):
                if child.returncode != 0:
                    answer["reason"] = "retirement_nonzero_exit"
                    break
                try:
                    answer["proof"] = parse_retirement(bytes(output), run_id, session, challenge)
                except ValueError:
                    answer["reason"] = "retirement_invalid_proof"
                    break
                answer.update(verified=True, reason="retirement_verified")
                break
            time.sleep(min(.01, max(0, deadline - time.monotonic())))
        else:
            answer["reason"] = "retirement_timeout"
    finally:
        if child.poll() is None:
            try:
                child.kill()  # Exact owned local verifier/SSH process only.
                answer["forced_local_kill"] = True
            except OSError:
                pass
        try:
            child.wait(timeout=.3)
        except subprocess.TimeoutExpired:
            answer.update(verified=False, proof=None, reason="retirement_reap_failed")
        for reader in readers:
            reader.join(timeout=.05)
        if not all(event.is_set() for event in done) or failure.is_set():
            answer.update(verified=False, proof=None, reason="retirement_output_error")
        for pipe in (child.stdout, child.stderr):
            try:
                pipe.close()
            except OSError:
                pass
    return answer
