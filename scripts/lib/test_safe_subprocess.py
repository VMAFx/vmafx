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
from collections.abc import Sequence
from pathlib import Path
from typing import Any, cast
from unittest import mock

from scripts.lib import safe_subprocess
from scripts.lib.safe_subprocess import (
    MAX_ARG_BYTES,
    CommandFailed,
    CommandOutputLimitExceeded,
    CommandResult,
    CommandTimedOut,
    CommandValidationError,
    executable_argument,
    resolve_executable,
    run,
    run_async,
    signal_process_group,
    terminate_windows_process_tree,
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

    @unittest.skipIf(os.name == "nt", "symbolic links need privileges on Windows")
    def test_symlink_keeps_the_invoked_name_as_argv0(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            link = Path(temporary) / "renamed-python"
            link.symlink_to(PYTHON)
            result = run(
                [str(link), "-c", "import sys; print(sys.orig_argv[0])"],
                allowed_executables=(str(link),),
                capture_output=True,
                text=True,
            )
        self.assertEqual(result.stdout.strip(), str(link))

    def test_argv_is_read_once(self) -> None:
        class Flipping(Sequence[str]):
            """argv whose first entry changes after it has been read once."""

            def __init__(self, items: list[str]) -> None:
                self.items, self.reads = items, 0

            def __len__(self) -> int:
                return len(self.items)

            def __getitem__(self, index: int) -> str:  # type: ignore[override]
                if index == 0:
                    self.reads += 1
                    if self.reads > 1:
                        return "swapped-name"
                return self.items[index]

        result = run(
            Flipping([PYTHON, "-c", "import sys; print(sys.orig_argv[0])"]),
            allowed_executables=ALLOW_PYTHON,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.stdout.strip(), PYTHON)

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

    def test_executable_argument_resolves_relative_paths_only(self) -> None:
        root = Path(PYTHON).parent
        # Positive: a path with a directory part becomes absolute under `root`,
        # and run() then accepts it where the relative form is refused.
        relative = Path(PYTHON).relative_to(root.parent).as_posix()
        self.assertIn("/", relative)
        resolved = executable_argument(relative, root.parent)
        self.assertEqual(resolved, str((root.parent / relative).resolve()))
        with self.assertRaises(CommandValidationError):
            run([relative, "-V"], allowed_executables=(relative,))
        self.assertEqual(run([resolved, "-V"], allowed_executables=(resolved,)).returncode, 0)
        # Boundary: a bare name stays for the PATH lookup; an absolute path stays.
        self.assertEqual(executable_argument("vmaf", root), "vmaf")
        self.assertEqual(executable_argument(PYTHON, "/elsewhere"), PYTHON)
        self.assertEqual(executable_argument("./vmaf", root), str((root / "vmaf").resolve()))
        self.assertEqual(
            executable_argument("../vmaf", root), str((root / ".." / "vmaf").resolve())
        )
        # Negative: a NUL byte is refused, as run() refuses it.
        with self.assertRaises(CommandValidationError):
            executable_argument("bin/x\0y", root)

    @unittest.skipUnless(hasattr(os, "symlink"), "symbolic links unavailable")
    @unittest.skipIf(os.name == "nt", "symbolic links need privileges on Windows")
    def test_executable_argument_preserves_symlink_name(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "vmaf").touch()
            (Path(tmp) / "vmafx").symlink_to("vmaf")
            # The CLI picks vmafx mode from argv0, so the alias must survive.
            self.assertEqual(executable_argument("./vmafx", root=tmp), str(Path(tmp) / "vmafx"))

    def test_resolve_executable_bare_name_and_relative_path(self) -> None:
        """Bare names on PATH and relative paths against root resolve to absolute files."""
        # Bare name on PATH -> absolute path that exists; PATH holds only the running interpreter
        python = Path(PYTHON)
        with mock.patch.dict(os.environ, {"PATH": str(python.parent)}):
            resolved_python = resolve_executable(python.name)
        self.assertEqual(resolved_python, PYTHON)
        self.assertTrue(Path(resolved_python).is_absolute())
        self.assertTrue(Path(resolved_python).is_file())
        self.assertTrue(os.access(resolved_python, os.X_OK))

        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            bin_dir = root / "bin"
            bin_dir.mkdir()
            tool = bin_dir / "runner"
            tool.touch()
            tool.chmod(0o755)

            # Relative path with a directory part -> resolved against root
            rel_path = Path("bin") / "runner"
            resolved_rel = resolve_executable(rel_path, root=root)
            self.assertEqual(resolved_rel, str(tool.resolve()))

            # Bare name in custom PATH
            with mock.patch.dict(os.environ, {"PATH": str(bin_dir)}):
                resolved_bare = resolve_executable("runner")
                self.assertEqual(resolved_bare, str(tool.resolve()))

    def test_resolve_executable_missing_name_raises(self) -> None:
        """Missing executable name raises CommandValidationError."""
        with self.assertRaises(CommandValidationError):
            resolve_executable("nonexistent-tool-binary-xyz")

    @unittest.skipIf(os.name == "nt", "POSIX execute permissions are not enforced on Windows")
    def test_resolve_executable_non_executable_file_raises(self) -> None:
        """Existing file without execute permission raises CommandValidationError."""
        with tempfile.TemporaryDirectory() as tmp:
            unexecutable = Path(tmp) / "plain_file.txt"
            unexecutable.touch()
            unexecutable.chmod(0o644)
            with self.assertRaises(CommandValidationError):
                resolve_executable(unexecutable)

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
class SignalTargetTests(unittest.TestCase):
    """The group-signal guard; ``os.killpg`` is a recorder here, so no signal is ever sent."""

    def test_refuses_targets_that_are_not_a_child_group(self) -> None:
        refused: list[object] = [
            0,
            1,
            -1,
            True,
            2.0,
            "5",
            None,
            mock.MagicMock(),
            mock.MagicMock().pid,
        ]
        refused.append(os.getpgrp())
        with mock.patch("os.killpg") as killpg:
            for target in refused:
                with self.subTest(target=repr(target)), self.assertRaises(ValueError):
                    signal_process_group(target, signal.SIGKILL)
        killpg.assert_not_called()

    def test_signals_a_child_group(self) -> None:
        target = os.getpgrp() + 1 if os.getpgrp() > 1 else 2
        with mock.patch("os.killpg") as killpg:
            self.assertTrue(signal_process_group(target, signal.SIGTERM))
        killpg.assert_called_once_with(target, signal.SIGTERM)

    def test_vanished_group_is_reported_not_raised(self) -> None:
        target = os.getpgrp() + 1 if os.getpgrp() > 1 else 2
        with mock.patch("os.killpg", side_effect=ProcessLookupError) as killpg:
            self.assertFalse(signal_process_group(target, signal.SIGKILL))
        killpg.assert_called_once_with(target, signal.SIGKILL)


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
                    signal_process_group(leader_pid, signal.SIGKILL)
                if not task.done():
                    task.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await task


class _StubTaskkillProcess:
    def __init__(self, returncode: int = 0) -> None:
        self.returncode = returncode

    async def wait(self) -> int:
        return self.returncode

    def kill(self) -> None:
        pass


class _StubProcessGroupChild:
    def __init__(self, pid: int = 4242) -> None:
        self.pid = pid
        self.returncode: int | None = None
        self.call_order: list[str] = []

    def terminate(self) -> None:
        self.call_order.append("terminate")

    def kill(self) -> None:
        self.call_order.append("kill")

    async def wait(self) -> int:
        self.returncode = 1
        self.call_order.append("wait")
        return 1


class WindowsTreeTerminationTests(unittest.TestCase):
    def test_windows_tree_termination_invokes_taskkill_with_exact_argv(self) -> None:
        stub_proc = _StubTaskkillProcess(returncode=0)
        exec_stub = mock.AsyncMock(return_value=stub_proc)
        with (
            mock.patch(
                "scripts.lib.safe_subprocess.resolve_executable",
                return_value="C:/Windows/System32/taskkill.exe",
            ),
            mock.patch("scripts.lib.safe_subprocess.asyncio.create_subprocess_exec", exec_stub),
        ):
            result = terminate_windows_process_tree(4242)

        self.assertTrue(result)
        exec_stub.assert_called_once()
        self.assertEqual(
            exec_stub.call_args[0],
            ("C:/Windows/System32/taskkill.exe", "/PID", "4242", "/T", "/F"),
        )

    def test_windows_tree_termination_nonzero_exit_status_returns_false(self) -> None:
        stub_proc = _StubTaskkillProcess(returncode=128)
        exec_stub = mock.AsyncMock(return_value=stub_proc)
        with (
            mock.patch(
                "scripts.lib.safe_subprocess.resolve_executable",
                return_value="C:/Windows/System32/taskkill.exe",
            ),
            mock.patch("scripts.lib.safe_subprocess.asyncio.create_subprocess_exec", exec_stub),
        ):
            result = terminate_windows_process_tree(4242)

        self.assertFalse(result)

    def test_windows_tree_termination_missing_taskkill_returns_false_and_starts_nothing(
        self,
    ) -> None:
        exec_stub = mock.AsyncMock()
        with (
            mock.patch(
                "scripts.lib.safe_subprocess.resolve_executable",
                side_effect=CommandValidationError("taskkill is unavailable"),
            ),
            mock.patch("scripts.lib.safe_subprocess.asyncio.create_subprocess_exec", exec_stub),
        ):
            result = terminate_windows_process_tree(4242)

        self.assertFalse(result)
        exec_stub.assert_not_called()

    def test_terminate_windows_process_tree_boundary_validation(self) -> None:
        invalid_targets: list[Any] = [0, -1, True, "12"]
        for target in invalid_targets:
            with self.subTest(target=repr(target)), self.assertRaises(CommandValidationError):
                terminate_windows_process_tree(target)

        stub_proc = _StubTaskkillProcess(returncode=0)
        exec_stub = mock.AsyncMock(return_value=stub_proc)
        with (
            mock.patch(
                "scripts.lib.safe_subprocess.resolve_executable",
                return_value="C:/Windows/System32/taskkill.exe",
            ),
            mock.patch("scripts.lib.safe_subprocess.asyncio.create_subprocess_exec", exec_stub),
        ):
            result = terminate_windows_process_tree(1)

        self.assertTrue(result)
        exec_stub.assert_called_once()
        self.assertEqual(
            exec_stub.call_args[0],
            ("C:/Windows/System32/taskkill.exe", "/PID", "1", "/T", "/F"),
        )

    def test_terminate_process_group_windows_terminates_tree_before_direct_child(self) -> None:
        fake = _StubProcessGroupChild(pid=4242)
        recorded_pids: list[int] = []

        async def record_tree_terminate(pid: int) -> bool:
            recorded_pids.append(pid)
            fake.call_order.append("tree_terminate")
            return True

        with (
            mock.patch("scripts.lib.safe_subprocess.os.name", "nt"),
            mock.patch(
                "scripts.lib.safe_subprocess._terminate_windows_process_tree",
                side_effect=record_tree_terminate,
            ),
        ):
            asyncio.run(safe_subprocess._terminate_process_group(cast(Any, fake)))

        self.assertEqual(recorded_pids, [4242])
        self.assertIn("terminate", fake.call_order)
        tree_idx = fake.call_order.index("tree_terminate")
        term_idx = fake.call_order.index("terminate")
        self.assertLess(tree_idx, term_idx)


if __name__ == "__main__":
    unittest.main()
