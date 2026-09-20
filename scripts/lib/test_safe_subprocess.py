#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Adversarial controls for the repository process-execution boundary."""

from __future__ import annotations

import asyncio
import contextlib
import os
import signal
import sys
import tempfile
import time
import unittest
from pathlib import Path
from typing import Any, cast

from scripts.lib.safe_subprocess import (
    MAX_ARG_BYTES,
    CommandFailed,
    CommandOutputLimitExceeded,
    CommandResult,
    CommandTimedOut,
    CommandValidationError,
    run,
    run_async,
)

PYTHON = str(Path(sys.executable).resolve(strict=True))
ALLOW_PYTHON = (PYTHON,)


class SafeSubprocessTests(unittest.TestCase):
    def python(self, source: str, **kwargs: Any) -> CommandResult:
        return cast(
            CommandResult,
            run([PYTHON, "-c", source], allowed_executables=ALLOW_PYTHON, **kwargs),
        )

    def test_text_and_binary_capture_are_distinct(self) -> None:
        binary = self.python("import sys; sys.stdout.buffer.write(b'\\xffok')", capture_output=True)
        self.assertEqual(binary.stdout, b"\xffok")
        text = self.python("print('ok')", capture_output=True, text=True)
        self.assertEqual(text.stdout, "ok\n")
        self.assertEqual(text.stderr, "")

    def test_uncaptured_streams_use_typed_empty_values(self) -> None:
        binary = self.python("pass")
        self.assertEqual(binary.stdout, b"")
        self.assertEqual(binary.stderr, b"")
        text = self.python("pass", text=True)
        self.assertEqual(text.stdout, "")
        self.assertEqual(text.stderr, "")

    def test_cwd_and_replacement_environment_are_explicit(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            result = self.python(
                "import os; from pathlib import Path; print(Path.cwd().name, os.environ['ONLY'])",
                cwd=temporary,
                env={"ONLY": "fixture"},
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.stdout, f"{Path(temporary).name} fixture\n")

    def test_check_preserves_the_result(self) -> None:
        with self.assertRaises(CommandFailed) as raised:
            self.python(
                "import sys; print('bad', file=sys.stderr); raise SystemExit(7)",
                capture_output=True,
                text=True,
                check=True,
            )
        self.assertEqual(raised.exception.result.returncode, 7)
        self.assertEqual(raised.exception.result.stderr, "bad\n")

    def test_stdin_is_closed_unless_bounded_input_is_supplied(self) -> None:
        result = self.python(
            "import sys; sys.stdout.write(sys.stdin.read())",
            input_data="payload",
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout, "payload")

    def test_unallowlisted_relative_and_nul_arguments_are_rejected(self) -> None:
        with self.assertRaises(CommandValidationError):
            run([PYTHON, "-V"], allowed_executables=("definitely-not-python",))
        with self.assertRaises(CommandValidationError):
            run(["../python", "-V"], allowed_executables=("python",))
        with self.assertRaises(CommandValidationError):
            self.python("print('ok')", env={"BAD\0KEY": "x"})
        with self.assertRaises(CommandValidationError):
            run([PYTHON, "x\0y"], allowed_executables=ALLOW_PYTHON)
        with self.assertRaises(CommandValidationError):
            run([PYTHON, "x" * (MAX_ARG_BYTES + 1)], allowed_executables=ALLOW_PYTHON)

    def test_capture_limit_terminates_a_flooding_child(self) -> None:
        with self.assertRaises(CommandOutputLimitExceeded) as raised:
            self.python(
                "import sys, time; sys.stdout.write('x' * 100000); sys.stdout.flush(); time.sleep(30)",
                capture_output=True,
                text=True,
                max_output_bytes=1024,
                timeout_seconds=5,
            )
        captured = raised.exception.stdout
        self.assertIsNotNone(captured)
        assert captured is not None
        self.assertEqual(len(captured), 1024)

    def test_timeout_terminates_the_child_and_returns_partial_output(self) -> None:
        started = time.monotonic()
        with self.assertRaises(CommandTimedOut) as raised:
            self.python(
                "import time; print('started', flush=True); time.sleep(30)",
                capture_output=True,
                text=True,
                timeout_seconds=0.2,
            )
        self.assertLess(time.monotonic() - started, 5)
        self.assertEqual(raised.exception.stdout, "started\n")

    @unittest.skipUnless(os.name == "posix", "process-group assertion requires POSIX signals")
    def test_timeout_terminates_descendants_in_the_same_group(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            pid_file = Path(temporary) / "child.pid"
            source = (
                "import pathlib, subprocess, sys, time; "
                "child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
                "time.sleep(30)"
            )
            with self.assertRaises(CommandTimedOut):
                self.python(source, timeout_seconds=0.3)
            child_pid = int(pid_file.read_text())
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                stat = Path(f"/proc/{child_pid}/stat")
                if not stat.exists() or stat.read_text().split()[2] == "Z":
                    break
                time.sleep(0.05)
            else:
                self.fail(f"descendant {child_pid} survived process-group timeout")

    @unittest.skipUnless(os.name == "posix", "process-group assertion requires POSIX signals")
    def test_timeout_kills_descendant_after_session_leader_exits(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            pid_file = Path(temporary) / "child.pid"
            source = (
                "import pathlib, subprocess, sys; "
                "child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(str(child.pid)); "
                "print('leader exited', flush=True)"
            )
            with self.assertRaises(CommandTimedOut) as raised:
                self.python(source, capture_output=True, text=True, timeout_seconds=0.3)
            self.assertEqual(raised.exception.stdout, "leader exited\n")
            child_pid = int(pid_file.read_text())
            deadline = time.monotonic() + 3
            while time.monotonic() < deadline:
                stat = Path(f"/proc/{child_pid}/stat")
                if not stat.exists() or stat.read_text().split()[2] == "Z":
                    break
                time.sleep(0.05)
            else:
                self.fail(f"descendant {child_pid} survived after its session leader exited")


@unittest.skipUnless(
    os.name == "posix" and Path("/proc").is_dir(),
    "process-group cancellation assertion requires procfs and POSIX signals",
)
class SafeSubprocessAsyncTests(unittest.IsolatedAsyncioTestCase):
    async def test_cancellation_terminates_process_group_and_reaps_leader(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            pid_file = Path(temporary) / "processes.pid"
            source = (
                "import os, pathlib, subprocess, sys, time; "
                "child=subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)']); "
                f"pathlib.Path({str(pid_file)!r}).write_text(f'{{os.getpid()}} {{child.pid}}'); "
                "time.sleep(30)"
            )
            task = asyncio.create_task(
                run_async(
                    [PYTHON, "-c", source],
                    allowed_executables=ALLOW_PYTHON,
                    timeout_seconds=30,
                )
            )
            leader_pid: int | None = None
            child_pid: int | None = None
            try:
                deadline = asyncio.get_running_loop().time() + 3
                while not pid_file.exists() and asyncio.get_running_loop().time() < deadline:
                    await asyncio.sleep(0.01)
                self.assertTrue(pid_file.exists(), "child did not publish its process IDs")
                leader_pid, child_pid = (int(value) for value in pid_file.read_text().split())

                task.cancel()
                with self.assertRaises(asyncio.CancelledError):
                    await task

                deadline = asyncio.get_running_loop().time() + 3
                while asyncio.get_running_loop().time() < deadline:
                    leader_exists = Path(f"/proc/{leader_pid}").exists()
                    child_stat = Path(f"/proc/{child_pid}/stat")
                    child_stopped = (
                        not child_stat.exists() or child_stat.read_text().split()[2] == "Z"
                    )
                    if not leader_exists and child_stopped:
                        break
                    await asyncio.sleep(0.05)
                else:
                    self.fail(
                        f"cancelled process group survived: leader={leader_pid}, child={child_pid}"
                    )
            finally:
                if leader_pid is not None:
                    with contextlib.suppress(ProcessLookupError):
                        os.killpg(leader_pid, signal.SIGKILL)
                if not task.done():
                    task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await task


if __name__ == "__main__":
    unittest.main()
