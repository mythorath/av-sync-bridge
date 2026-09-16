# SPDX-License-Identifier: GPL-2.0-or-later
"""Actual native pipe/lease tests; generated control only, no media or network."""
import argparse
import os
from pathlib import Path
import subprocess
import sys
import time
import unittest

EXECUTABLE = os.environ.get("AVSYNC_CONTROL_FIXTURE")


@unittest.skipUnless(EXECUTABLE, "native fixture supplied by CTest")
class NativeControlPipeTests(unittest.TestCase):
    def child(self):
        child = subprocess.Popen([EXECUTABLE], stdin=subprocess.PIPE,
                                 stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(self.close_child, child)
        return child

    @staticmethod
    def close_child(child):
        if child.poll() is None:
            child.kill()
        child.wait(timeout=3)
        for stream in (child.stdin, child.stdout, child.stderr):
            if stream and not stream.closed:
                stream.close()

    def check_result(self, child, expected, code):
        child.wait(timeout=8)
        self.assertEqual(child.stdout.read().decode().splitlines(),
                         ["CONTROL_PIPE_READY", expected])
        self.assertEqual(child.stderr.read(), b"")
        self.assertEqual(child.returncode, code)

    def test_stop(self):
        child = self.child()
        child.stdin.write(b"AVSYNC_STOP\n")
        child.stdin.flush()
        self.check_result(child, "control_stopped", 0)

    def test_eof(self):
        child = self.child()
        child.stdin.close()
        self.check_result(child, "control_eof", 1)

    def test_expiry_without_eof(self):
        child = self.child()
        self.check_result(child, "control_expired", 1)

    def test_keepalive_then_fragmented_stop(self):
        child = self.child()
        for _ in range(6):
            child.stdin.write(b"AVSYNC_KEEPALIVE\n")
            child.stdin.flush()
            time.sleep(1)
            self.assertIsNone(child.poll())
        for part in (b"AVSYNC_", b"STOP", b"\n"):
            child.stdin.write(part)
            child.stdin.flush()
        self.check_result(child, "control_stopped", 0)

    def test_invalid_and_overlong(self):
        for payload in (b"UNKNOWN\n", b"A" * 1024, b"AVSYNC_KEEPALIVE\r\n"):
            with self.subTest(payload=payload[:20]):
                child = self.child()
                child.stdin.write(payload)
                child.stdin.flush()
                self.check_result(child, "control_invalid", 1)

    def test_nonpipe_rejected(self):
        child = subprocess.run([EXECUTABLE], stdin=subprocess.DEVNULL,
                               capture_output=True, timeout=3)
        self.assertEqual(child.returncode, 2)
        self.assertEqual(child.stdout, b"")
        self.assertIn(b"Control stdin must be a pipe", child.stderr)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--executable", required=True, type=Path)
    args, remaining = parser.parse_known_args()
    EXECUTABLE = str(args.executable.resolve(strict=True))
    # unittest's class decorator ran before command-line parsing.
    NativeControlPipeTests.__unittest_skip__ = False
    unittest.main(argv=[sys.argv[0], *remaining])
