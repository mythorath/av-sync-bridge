# SPDX-License-Identifier: GPL-2.0-or-later
"""No real processes, cgroups, devices or services are started by these tests."""
import json
from pathlib import Path
import sys
import unittest
from unittest.mock import Mock, patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "tools"))
import check_remote_receiver_control as check


class CheckerTests(unittest.TestCase):
    def child(self):
        child = Mock()
        child.poll.return_value = None
        child.stdout.fileno.return_value = 10
        return child

    def ready(self, chunks):
        child = self.child()
        with patch.object(check.select, "select", return_value=([child.stdout], [], [])), \
                patch.object(check.os, "read", side_effect=chunks):
            check.wait_ready(child, "1234", .1)

    def test_exact_receiver_and_fresh_valid_clock(self):
        line = b'AVSYNC_CONTROL {"schema":1,"event":"receiver_ready","clock_epoch":"18446744073709551615","sender_session":"1234"}\n'
        self.ready([line[:20], line[20:]])
        for value in (line.replace(b'1234', b'1235'), line.replace(b'18446744073709551615', b'0'),
                      line.replace(b'receiver_ready', b'sender_started'), b"noise\n", line + line):
            with self.subTest(value=value), self.assertRaises((RuntimeError, ValueError)):
                self.ready([value])

    def test_eof_and_output_limits_are_not_ready(self):
        for chunks in ([b""], [b"x" * 2049]):
            with self.assertRaises(RuntimeError):
                self.ready(chunks)

    def test_dead_child_or_deadline_is_not_ready(self):
        child = self.child()
        child.poll.return_value = 1
        with self.assertRaises(RuntimeError):
            check.wait_ready(child, "1234", .1)
        child.poll.return_value = None
        with patch.object(check.time, "monotonic", side_effect=[0, 2]), self.assertRaises(TimeoutError):
            check.wait_ready(child, "1234", .1)

    def test_proc_stat_handles_parentheses_in_name(self):
        # starttime is field22, index19 after the final parenthesized comm field.
        text = "123 (odd ) name) " + " ".join(["S"] + ["0"] * 18 + ["999"] + ["0"] * 8)
        with patch.object(Path, "read_text", return_value=text):
            self.assertEqual(check.start_ticks(123), 999)


if __name__ == "__main__":
    unittest.main()
