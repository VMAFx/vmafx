#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, negative and boundary cases for scripts/ci/run_actionlint.py (ADR-2199)."""

from __future__ import annotations

import os
import signal
import stat
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import run_actionlint
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
WRAPPER = ROOT / "scripts" / "ci" / "run_actionlint.py"


class Wrapper(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-actionlint-test-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)

    def fake(self, body: str) -> Path:
        path = self.directory / "actionlint"
        path.write_text(f"#!/bin/sh\n{body}\n", encoding="utf-8")
        path.chmod(path.stat().st_mode | stat.S_IXUSR)
        return path

    def run_wrapper(
        self,
        binary: Path | None,
        *args: str,
        timeout: str = "30",
        dump_wait_s: str = "2",
    ) -> tuple[int, str, str, float]:
        env = {
            "PATH": "/usr/bin:/bin",
            "ACTIONLINT_TIMEOUT_S": timeout,
            "ACTIONLINT_DUMP_DIR": str(self.directory / "dumps"),
            "ACTIONLINT_DUMP_WAIT_S": dump_wait_s,
        }
        if binary is not None:
            env["ACTIONLINT_BIN"] = str(binary)
        started = time.monotonic()
        result = run_command(
            [sys.executable, str(WRAPPER), *args],
            allowed_executables=(sys.executable,),
            env=env,
            text=True,
            capture_output=True,
            check=False,
            timeout_seconds=60,
        )
        return result.returncode, str(result.stdout), str(result.stderr), time.monotonic() - started

    def test_a_clean_run_passes_through(self) -> None:
        code, out, _err, _t = self.run_wrapper(
            self.fake('echo "args: $*"; exit 0'), "a.yml", "b.yml"
        )
        self.assertEqual(code, 0)
        self.assertIn("args: a.yml b.yml", out)

    def test_findings_keep_their_exit_status_and_output(self) -> None:
        code, out, err, _t = self.run_wrapper(
            self.fake('echo "a.yml:1:1: bad" ; echo warn >&2; exit 1')
        )
        self.assertEqual(code, 1)
        self.assertIn("a.yml:1:1: bad", out)
        self.assertIn("warn", err)

    def test_a_hang_is_a_loud_failure_within_the_deadline(self) -> None:
        """Planted hang: the fake never exits. Without the deadline this test would not return."""
        marker = self.directory / "child.pid"
        binary = self.fake(f'sleep 600 &\necho $! > "{marker}"\nwait')
        code, out, err, elapsed = self.run_wrapper(binary, "x.yml", timeout="2")
        self.assertEqual(code, run_actionlint.EXIT_TIMEOUT)
        self.assertEqual(out, "")
        self.assertIn("did not finish within 2 s", err)
        self.assertIn("FAILURE, not a pass", err)
        self.assertLess(elapsed, 20)
        child = int(marker.read_text().strip())
        time.sleep(0.5)
        with self.assertRaises(ProcessLookupError):
            os.kill(child, 0)  # the whole process group was terminated

    def test_the_goroutine_dump_is_kept_and_named(self) -> None:
        """SIGQUIT at the deadline makes a Go program print its stacks; the wrapper saves them."""
        binary = self.fake(
            'trap \'echo "SIGQUIT: quit" >&2; echo "goroutine 1 [sync.WaitGroup.Wait, 5 minutes]:" >&2; '
            'echo "os.(*File).Write process.go:32" >&2; exit 2\' QUIT\n'
            # `wait` returns as soon as the trapped signal arrives (a foreground `sleep` holds
            # the trap until it ends), and the background sleep keeps no pipe open. A foreground
            # sleep plus a 2 s dump wait failed under the merge train's loaded tooling run.
            "while true; do sleep 1 >/dev/null 2>&1 & wait $!; done"
        )
        code, _out, err, _t = self.run_wrapper(binary, "x.yml", timeout="2", dump_wait_s="5")
        self.assertEqual(code, run_actionlint.EXIT_TIMEOUT)
        dumps = list((self.directory / "dumps").glob("actionlint-hang-*.txt"))
        self.assertEqual(len(dumps), 1, err)
        self.assertIn(str(dumps[0]), err)
        text = dumps[0].read_text(encoding="utf-8")
        self.assertIn("goroutine 1", text)
        self.assertIn("process.go:32", text)

    def test_the_dump_is_kept_when_started_with_sigquit_ignored(self) -> None:
        """A background job of a non-interactive shell starts with SIGQUIT ignored."""
        previous = signal.signal(signal.SIGQUIT, signal.SIG_IGN)
        self.addCleanup(signal.signal, signal.SIGQUIT, previous)
        self.test_the_goroutine_dump_is_kept_and_named()

    def test_a_hang_without_a_dump_says_so(self) -> None:
        """A program that ignores SIGQUIT is still terminated and the message admits no dump."""
        binary = self.fake("trap '' QUIT\nwhile true; do sleep 1; done")
        code, _out, err, elapsed = self.run_wrapper(binary, "x.yml", timeout="2")
        self.assertEqual(code, run_actionlint.EXIT_TIMEOUT)
        self.assertIn("No goroutine dump could be saved", err)
        self.assertLess(elapsed, 40)

    def test_a_missing_binary_is_not_a_pass(self) -> None:
        code, _out, err, _t = self.run_wrapper(None)
        self.assertEqual(code, run_actionlint.EXIT_MISSING)
        self.assertIn("not on PATH", err)

    def test_a_bad_deadline_is_refused(self) -> None:
        for value in ("0", "-3", "soon"):
            with self.subTest(value=value):
                code, _out, _err, _t = self.run_wrapper(self.fake("exit 0"), timeout=value)
                self.assertNotEqual(code, 0)


class Diagnosis(unittest.TestCase):
    def test_a_shrunk_pipe_names_the_cause(self) -> None:
        text = run_actionlint.diagnosis(90, 8192)
        self.assertIn("fs.pipe-user-pages-soft", text)
        self.assertIn("8192", text)
        self.assertIn("This is the cause", text)

    def test_a_normal_pipe_asks_for_a_dump(self) -> None:
        text = run_actionlint.diagnosis(90, run_actionlint.FULL_PIPE_BYTES)
        self.assertIn("goroutine dump", text)
        self.assertNotIn("This is the cause", text)

    def test_an_unreadable_capacity_is_said(self) -> None:
        self.assertIn("could not be read", run_actionlint.diagnosis(90, None))

    def test_the_capacity_probe_reads_a_pipe(self) -> None:
        capacity = run_actionlint.pipe_capacity()
        if sys.platform.startswith("linux"):
            self.assertIsNotNone(capacity)
            assert capacity is not None
            self.assertGreaterEqual(capacity, 4096)


class Wiring(unittest.TestCase):
    def test_the_hook_and_the_make_target_go_through_the_wrapper(self) -> None:
        config = (ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8")
        hook = config.split("  - repo: https://github.com/rhysd/actionlint\n", 1)[1].split(
            "\n\n", 1
        )[0]
        self.assertIn("entry: python3 scripts/ci/run_actionlint.py", hook)
        makefile = (ROOT / "Makefile").read_text(encoding="utf-8")
        target = makefile.split("\nlint-actions:\n", 1)[1].split("\n\n", 1)[0]
        self.assertIn("python3 scripts/ci/run_actionlint.py", target)
        self.assertNotIn("\t@actionlint\n", target)


if __name__ == "__main__":
    unittest.main()
