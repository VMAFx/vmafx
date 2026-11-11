#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Adversarial controls for the repository process-execution boundary."""

from __future__ import annotations

import asyncio
import contextlib
import ctypes
import errno
import os
import signal
import sys
import tempfile
import time
import unittest
from collections.abc import Iterator, Sequence
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

    @unittest.skipIf(os.name == "nt", "symbolic links need privileges on Windows")
    def test_launch_path_keeps_the_links_the_caller_named(self) -> None:
        """The file is launched by the named path, not its resolution, absolute or on PATH."""
        with tempfile.TemporaryDirectory() as temporary:
            real, links = Path(temporary) / "real", Path(temporary) / "links"
            real.mkdir()
            links.mkdir()
            links.chmod(0o755)
            script = real / "tool"
            # A script's interpreter receives the path the kernel executed as $0.
            script.write_text("#!/bin/sh\nprintf '%s' \"$0\"\n", encoding="utf-8")
            script.chmod(0o755)
            link = links / "tool"
            link.symlink_to(script)
            absolute = run([str(link)], allowed_executables=(str(link),), capture_output=True)
            bare = run(
                ["tool"],
                allowed_executables=("tool",),
                env={"PATH": str(links)},
                capture_output=True,
            )
        self.assertEqual(absolute.stdout, os.fsencode(link))
        self.assertEqual(bare.stdout, os.fsencode(link))
        self.assertEqual(absolute.argv[0], str(link))

    @unittest.skipIf(os.name == "nt", "a Windows venv copies its interpreter, it does not link it")
    def test_venv_interpreter_link_keeps_its_venv(self) -> None:
        """A venv's python link runs with the venv's prefix (macOS lost it in #3386)."""
        with tempfile.TemporaryDirectory() as temporary:
            venv = Path(temporary) / "venv"
            (venv / "bin").mkdir(parents=True)
            (venv / "pyvenv.cfg").write_text(
                f"home = {Path(PYTHON).parent}\ninclude-system-site-packages = false\n",
                encoding="utf-8",
            )
            interpreter = venv / "bin" / "python"
            interpreter.symlink_to(PYTHON)
            result = run(
                [str(interpreter), "-I", "-c", "import sys; print(sys.prefix)"],
                allowed_executables=(str(interpreter),),
                capture_output=True,
                text=True,
            )
            self.assertEqual(Path(result.stdout.strip()).resolve(), venv.resolve())

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
        """Record the stub returncode for taskkill simulation."""
        self.returncode = returncode

    async def wait(self) -> int:
        """Return the recorded taskkill exit status."""
        return self.returncode

    def kill(self) -> None:
        """Stub kill invocation on the taskkill process."""


class _StubProcessGroupChild:
    def __init__(self, pid: int = 4242) -> None:
        """Record the child PID and initialize call order tracking."""
        self.pid = pid
        self.returncode: int | None = None
        self.call_order: list[str] = []

    def terminate(self) -> None:
        """Record terminate invocation in call order."""
        self.call_order.append("terminate")

    def kill(self) -> None:
        """Record kill invocation in call order."""
        self.call_order.append("kill")

    async def wait(self) -> int:
        """Record wait invocation and set returncode."""
        self.returncode = 1
        self.call_order.append("wait")
        return 1


class WindowsTreeTerminationTests(unittest.TestCase):
    def test_windows_tree_termination_invokes_taskkill_with_exact_argv(self) -> None:
        """Assert that taskkill is invoked with exact PID and flags."""
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
        """Assert that a non-zero taskkill exit status returns False."""
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
        """Assert that missing taskkill returns False and starts nothing."""
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
        """Assert that invalid process IDs are rejected and positive PID succeeds."""
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
        """Assert that process tree is terminated before the direct child."""
        fake = _StubProcessGroupChild(pid=4242)
        recorded_pids: list[int] = []

        async def record_tree_terminate(pid: int) -> bool:
            """Record the tree termination PID and call order."""
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


class DarwinAclTests(unittest.TestCase):
    """Replay native ACL return conventions without requiring a macOS host."""

    def fake_libc(self, tags: Sequence[int]) -> mock.Mock:
        """Supply native-shaped ACL functions and bounded synthetic entry enumeration."""
        libc = mock.Mock()
        position = 0

        def get_entry(_acl: int, _selector: int, entry: Any) -> int:
            """Return entries in order, then the native EINVAL end marker."""
            nonlocal position
            if position == len(tags):
                ctypes.set_errno(errno.EINVAL)
                return -1
            position += 1
            ctypes.cast(entry, ctypes.POINTER(ctypes.c_void_p))[0] = position
            return 0

        def get_tag(_entry: Any, tag: Any) -> int:
            """Write the enumerated tag through the native output pointer."""
            ctypes.cast(tag, ctypes.POINTER(ctypes.c_int))[0] = tags[position - 1]
            return 0

        libc.acl_get_link_np.return_value = 42
        libc.acl_get_entry.side_effect = get_entry
        libc.acl_get_tag_type.side_effect = get_tag
        libc.acl_free.return_value = 0
        self.factory = self.enterContext(mock.patch.object(ctypes, "CDLL", return_value=libc))
        return libc

    def test_missing_acl_is_not_a_grant(self) -> None:
        """ENOENT with a NULL ACL is the native representation of no extended ACL."""
        libc = self.fake_libc([])
        libc.acl_get_link_np.side_effect = lambda *_: ctypes.set_errno(errno.ENOENT)
        self.assertFalse(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_free.assert_not_called()

    def test_null_acl_errors_fail_closed(self) -> None:
        """Unreadable or unspecified NULL results cannot authorize a directory."""
        libc = self.fake_libc([])
        for error in (errno.EACCES, 0):
            with self.subTest(error=error):
                libc.acl_get_link_np.side_effect = lambda *_, error=error: ctypes.set_errno(error)
                self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_free.assert_not_called()

    def test_library_or_symbol_unavailable_fails_closed(self) -> None:
        """Library and symbol lookup errors cannot silently bypass ACL inspection."""
        for error in (OSError("planted load error"), AttributeError("acl_get_link_np")):
            with (
                self.subTest(error=error),
                mock.patch.object(ctypes, "CDLL", side_effect=error),
            ):
                self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))

    def test_missing_symbol_fails_closed_before_allocating_acl(self) -> None:
        """An incomplete native ACL interface cannot leave a live allocation behind."""
        libc = self.fake_libc([])
        del libc.acl_get_entry
        self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_get_link_np.assert_not_called()
        libc.acl_free.assert_not_called()

    def test_empty_and_deny_only_acls_are_not_grants(self) -> None:
        """Zero through exactly the native entry bound of deny entries remain trusted."""
        self.assertEqual(safe_subprocess.MAX_DARWIN_ACL_ENTRIES, 128)
        for count in (0, 1, 128):
            with self.subTest(count=count):
                libc = self.fake_libc([2] * count)
                self.assertFalse(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
                libc.acl_free.assert_called_once_with(42)
                selectors = [call.args[1] for call in libc.acl_get_entry.call_args_list]
                self.assertEqual(selectors, [0] + [-1] * count)

    def test_allow_or_unknown_tag_fails_closed(self) -> None:
        """Any allow entry or unrecognized tag prevents named-path authorization."""
        for tags in ([1], [2, 1], [99]):
            with self.subTest(tags=tags):
                libc = self.fake_libc(tags)
                self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
                libc.acl_free.assert_called_once_with(42)

    def test_tag_read_failure_fails_closed_and_frees_acl(self) -> None:
        """Unreadable tags do not hide permissions or leak the native ACL allocation."""
        libc = self.fake_libc([2])
        libc.acl_get_tag_type.side_effect = None
        libc.acl_get_tag_type.return_value = -1
        self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_free.assert_called_once_with(42)

    def test_entry_read_failure_fails_closed_and_frees_acl(self) -> None:
        """Only the native EINVAL end marker terminates a successful enumeration."""
        libc = self.fake_libc([2])

        def denied_entry(*_args: Any) -> int:
            """Plant an entry read error distinct from the end marker."""
            ctypes.set_errno(errno.EACCES)
            return -1

        libc.acl_get_entry.side_effect = denied_entry
        self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_free.assert_called_once_with(42)

    def test_acl_over_bound_fails_closed_and_frees_acl(self) -> None:
        """The first extra entry rejects the ACL without reading beyond the bound."""
        libc = self.fake_libc([2] * 129)
        self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        self.assertEqual(libc.acl_get_entry.call_count, 129)
        self.assertEqual(libc.acl_get_tag_type.call_count, 128)
        libc.acl_free.assert_called_once_with(42)

    def test_acl_free_failure_fails_closed(self) -> None:
        """An unsuccessful native cleanup cannot report complete ACL inspection."""
        libc = self.fake_libc([2])
        libc.acl_free.return_value = -1
        self.assertTrue(safe_subprocess._darwin_acl_allows_entries(Path("/directory")))
        libc.acl_free.assert_called_once_with(42)

    def test_native_signatures_and_encoded_path(self) -> None:
        """Native pointers retain their width and paths use filesystem encoding."""
        libc = self.fake_libc([])
        directory = Path("/directory-ä")
        self.assertFalse(safe_subprocess._darwin_acl_allows_entries(directory))
        self.factory.assert_called_once_with(None, use_errno=True)
        libc.acl_get_link_np.assert_called_once_with(os.fsencode(directory), 0x100)
        self.assertEqual(libc.acl_get_link_np.argtypes, [ctypes.c_char_p, ctypes.c_int])
        self.assertIs(libc.acl_get_link_np.restype, ctypes.c_void_p)
        self.assertEqual(
            libc.acl_get_entry.argtypes,
            [ctypes.c_void_p, ctypes.c_int, ctypes.POINTER(ctypes.c_void_p)],
        )
        self.assertIs(libc.acl_get_entry.restype, ctypes.c_int)
        self.assertEqual(
            libc.acl_get_tag_type.argtypes, [ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)]
        )
        self.assertIs(libc.acl_get_tag_type.restype, ctypes.c_int)
        self.assertEqual(libc.acl_free.argtypes, [ctypes.c_void_p])
        self.assertIs(libc.acl_free.restype, ctypes.c_int)
        libc.acl_free.assert_called_once_with(42)


@unittest.skipUnless(sys.platform == "darwin", "native extended ACLs require macOS")
class DarwinAclLinkTests(unittest.TestCase):
    """Validate real directory ACLs through the executable launch selector on macOS."""

    def setUp(self) -> None:
        """Create a mode-protected link directory with no inherited ACL entries."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.root.chmod(0o755)
        self.link = self.root / "python-link"
        self.link.symlink_to(PYTHON)
        run(["/bin/chmod", "-N", str(self.root)], allowed_executables=("/bin/chmod",), check=True)
        self.addCleanup(
            run,
            ["/bin/chmod", "-N", str(self.root)],
            allowed_executables=("/bin/chmod",),
            check=True,
        )

    def assert_launch(self, expected: str) -> None:
        """Require the actual launch path chosen for the real directory permissions."""
        result = run([str(self.link), "-V"], allowed_executables=ALLOW_PYTHON, check=True)
        self.assertEqual(result.argv[0], expected)

    def test_no_acl_keeps_named_launch(self) -> None:
        """A trusted mode without an extended ACL preserves the interpreter link."""
        self.assert_launch(str(self.link))

    def test_allow_entries_launch_resolved_file(self) -> None:
        """Real add/delete-child grants force resolved launch despite mode 0755."""
        run(
            ["/bin/chmod", "+a", "everyone allow add_file,delete_child", str(self.root)],
            allowed_executables=("/bin/chmod",),
            check=True,
        )
        self.assertEqual(self.root.stat().st_mode & 0o777, 0o755)
        self.assert_launch(PYTHON)

    def test_deny_only_acl_keeps_named_launch(self) -> None:
        """A real deny entry grants no right to replace the interpreter link."""
        run(
            ["/bin/chmod", "+a", "everyone deny delete", str(self.root)],
            allowed_executables=("/bin/chmod",),
            check=True,
        )
        self.assert_launch(str(self.link))


@unittest.skipUnless(hasattr(os, "getuid"), "POSIX link ownership requires os.getuid")
class ExecutableLinkTrustTests(unittest.TestCase):
    """Exercise trusted launch paths and bounded traversal without elevated privileges."""

    def setUp(self) -> None:
        """Create an owned link without inherited temporary-directory links."""
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name).resolve()
        self.root.chmod(0o755)
        self.link = self.root / "python-link"
        self.link.symlink_to(PYTHON)
        self.shell_target: Path | None = None

    def assert_resolved_launch(self, launch: Path, resolved: Path = Path(PYTHON)) -> None:
        """Require the allowlisted resolved file while preserving the caller's argument name."""
        allowed = (str(resolved),)
        self.assertEqual(
            safe_subprocess._checked_executable(str(launch), allowed, ""),
            (str(resolved), resolved),
        )
        result = run(
            [str(launch), "-c", "import sys; print(sys.orig_argv[0])"],
            allowed_executables=allowed,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.argv[0], str(resolved))
        expected = resolved if resolved == self.shell_target else launch
        self.assertEqual(result.stdout.strip(), str(expected))

    def same_directory_target(self, directory: Path) -> Path:
        """Copy an interpreter beside its link; use a shell fixture if it cannot relocate."""
        target = directory / "python-file"
        target.write_bytes(Path(PYTHON).read_bytes())
        target.chmod(0o755)
        result = run(
            [str(target), "-c", "pass"],
            allowed_executables=(str(target),),
            capture_output=True,
        )
        if result.returncode != 0:
            target.write_text('#!/bin/sh\nprintf "%s\\n" "$0"\n', encoding="utf-8")
            self.shell_target = target
        return target

    def assert_named_launch(self, launch: Path, resolved: Path) -> None:
        """Require named-path selection and observe that path in the launched process."""
        allowed = (str(resolved),)
        self.assertEqual(
            safe_subprocess._checked_executable(str(launch), allowed, ""),
            (str(launch), resolved),
        )
        result = run(
            [str(launch), "-c", "import sys; print(sys.orig_argv[0])"],
            allowed_executables=allowed,
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.argv[0], str(launch))
        self.assertEqual(result.stdout.strip(), str(launch))

    def same_directory_link(self, mode: int = 0o775) -> Path:
        """Place a runnable file beside the owned link in the requested directory mode."""
        target = self.same_directory_target(self.root)
        self.link.unlink()
        self.link.symlink_to(target.name)
        self.root.chmod(mode)
        return target

    @contextlib.contextmanager
    def darwin_flags(self, target: Path, flags: int, link_flags: int = 0) -> Iterator[None]:
        """Inject entry flags through the real macOS reader without changing host flags."""
        reader = safe_subprocess._darwin_entry_protected

        def read_flags(entry: Path) -> bool | None:
            """Supply only the flag reader's lstat result; retain real ownership metadata."""
            value = flags if entry == target else link_flags
            with mock.patch.object(os, "lstat", return_value=mock.Mock(st_flags=value)):
                return reader(entry)

        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.object(safe_subprocess, "_darwin_entry_protected", side_effect=read_flags),
            mock.patch.object(safe_subprocess, "_darwin_acl_allows_entries", return_value=False),
        ):
            yield

    @contextlib.contextmanager
    def linux_flags(
        self, flags: int = 0, error: int | None = None, machine: str = "x86_64"
    ) -> Iterator[mock.MagicMock]:
        """Inject Linux ioctl replies or errors on a selected architecture, offline."""
        reply = flags.to_bytes(ctypes.sizeof(ctypes.c_long), sys.byteorder)
        failure = OSError(error, "planted flag-read error") if error is not None else None
        with (
            mock.patch.object(sys, "platform", "linux"),
            mock.patch.object(safe_subprocess.platform, "machine", return_value=machine),
            mock.patch.object(
                safe_subprocess.fcntl, "ioctl", return_value=reply, side_effect=failure
            ) as reader,
        ):
            yield reader

    def test_darwin_protected_file_unprotected_link_launches_resolved_file(self) -> None:
        """Each unlink/rename protection bit defeats the writable-directory exemption."""
        target = self.same_directory_link()
        for flag in (0x2, 0x4, 0x20000, 0x40000, 0x100000, 0x80000):
            with self.subTest(flag=hex(flag)), self.darwin_flags(target, flag):
                self.assert_resolved_launch(self.link, target)

    def test_darwin_matching_protection_keeps_named_launch(self) -> None:
        """Unprotected pairs and pairs protected by each flag have matching exposure."""
        target = self.same_directory_link()
        for flag in (0, 0x2, 0x4, 0x20000, 0x40000, 0x100000, 0x80000):
            with self.subTest(flag=hex(flag)), self.darwin_flags(target, flag, flag):
                self.assert_named_launch(self.link, target)

    def test_darwin_protected_file_in_owned_directory_keeps_named_launch(self) -> None:
        """Owned mode 0755 directories still protect links to immutable files."""
        target = self.same_directory_link(0o755)
        with self.darwin_flags(target, 0x2):
            self.assert_named_launch(self.link, target)

    def test_darwin_unreadable_flags_use_directory_trust(self) -> None:
        """Missing or unreadable macOS flags deny only the directory exemption."""
        target = self.same_directory_link()
        reader = safe_subprocess._darwin_entry_protected
        for failure in (AttributeError("st_flags"), OSError(errno.EIO, "planted flags")):
            with (
                self.subTest(failure=type(failure).__name__),
                mock.patch.object(sys, "platform", "darwin"),
                mock.patch.object(
                    safe_subprocess, "_darwin_acl_allows_entries", return_value=False
                ),
                mock.patch.object(safe_subprocess, "_darwin_entry_protected", return_value=None),
            ):
                with mock.patch.object(os, "lstat", side_effect=failure):
                    # Exercise the actual reader separately from link ownership lookups.
                    self.assertIsNone(reader(self.link))
                self.root.chmod(0o775)
                self.assert_resolved_launch(self.link, target)
                self.root.chmod(0o755)
                self.assert_named_launch(self.link, target)

    def test_darwin_unknown_entry_protection_denies_exemption(self) -> None:
        """A protected peer cannot compensate for unknown resolved-file or link flags."""
        target = self.same_directory_link()
        for file_flags, link_flags in ((None, True), (True, None)):
            with (
                self.subTest(file_flags=file_flags, link_flags=link_flags),
                mock.patch.object(sys, "platform", "darwin"),
                mock.patch.object(
                    safe_subprocess,
                    "_darwin_entry_protected",
                    side_effect=lambda entry, resolved=file_flags, link=link_flags: (
                        resolved if entry == target else link
                    ),
                ),
            ):
                self.assert_resolved_launch(self.link, target)

    def test_darwin_protected_file_checks_directory_acl(self) -> None:
        """When the exemption is denied, ACL grants can defeat owned mode 0755."""
        target = self.same_directory_link(0o755)
        with (
            self.darwin_flags(target, 0x2),
            mock.patch.object(safe_subprocess, "_darwin_acl_allows_entries", return_value=True),
        ):
            self.assert_resolved_launch(self.link, target)

    def test_linux_protected_file_launches_resolved_file(self) -> None:
        """Immutable and append-only inode flags make Linux symlinks less protected."""
        target = self.same_directory_link()
        for flag in (0x10, 0x20):
            with self.subTest(flag=hex(flag)), self.linux_flags(flag):
                self.assert_resolved_launch(self.link, target)
        self.root.chmod(0o755)
        with self.linux_flags(0x10):
            self.assert_named_launch(self.link, target)

    def test_linux_unsupported_flags_keep_named_launch(self) -> None:
        """Filesystems without inode flags give the file and link matching exposure."""
        target = self.same_directory_link()
        for error in (errno.ENOTTY, errno.EOPNOTSUPP, errno.ENOSYS, errno.EINVAL):
            with self.subTest(error=error), self.linux_flags(error=error):
                self.assert_named_launch(self.link, target)

    def test_linux_unreadable_flags_launch_resolved_file(self) -> None:
        """I/O and permission failures cannot authorize a writable-directory exemption."""
        target = self.same_directory_link()
        for error in (errno.EIO, errno.EACCES):
            with self.subTest(error=error), self.linux_flags(error=error):
                self.assert_resolved_launch(self.link, target)

    def test_linux_unknown_ioctl_architecture_uses_directory_trust(self) -> None:
        """Unknown ioctl layouts deny the exemption, retaining owned-directory trust."""
        target = self.same_directory_link()
        for machine in ("mips64", "arm", "armv7b"):
            with self.subTest(machine=machine), self.linux_flags(machine=machine) as reader:
                self.root.chmod(0o775)
                self.assert_resolved_launch(self.link, target)
                self.root.chmod(0o755)
                self.assert_named_launch(self.link, target)
            reader.assert_not_called()

    def test_linux_flag_request_and_descriptor_are_bounded(self) -> None:
        """Use the native-long generic request and a no-follow, nonblocking descriptor."""
        target = self.same_directory_link()
        original = os.open
        for machine in ("x86_64", "aarch64", "riscv64", "armv6l", "armv7l", "armv8l", "i686"):
            with (
                self.subTest(machine=machine),
                self.linux_flags(machine=machine) as reader,
                mock.patch.object(os, "open", wraps=original) as opened,
            ):
                self.assertFalse(safe_subprocess._linux_file_protected(target))
                opened.assert_called_once_with(
                    target, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC
                )
                size = ctypes.sizeof(ctypes.c_long)
                request = (2 << 30) | (size << 16) | (ord("f") << 8) | 1
                reader.assert_called_once_with(mock.ANY, request, bytes(size))
                with self.assertRaises(OSError):
                    os.fstat(reader.call_args.args[0])

    def test_linux_unreadable_descriptor_uses_directory_trust(self) -> None:
        """Even unsupported-ioctl errnos during open leave protection unknown."""
        target = self.same_directory_link()
        original = os.open

        def unreadable(path: Any, *args: Any, **kwargs: Any) -> int:
            """Refuse only the protected-file descriptor, preserving child stream setup."""
            if path == target:
                raise OSError(errno.ENOTTY, "planted open")
            return original(path, *args, **kwargs)

        with (
            self.linux_flags() as reader,
            mock.patch.object(os, "open", side_effect=unreadable),
        ):
            self.assert_resolved_launch(self.link, target)
            self.root.chmod(0o755)
            self.assert_named_launch(self.link, target)
        reader.assert_not_called()

    def test_linux_malformed_flag_reply_uses_directory_trust(self) -> None:
        """Truncated or non-byte replies cannot authorize the directory exemption."""
        target = self.same_directory_link()
        for reply in (b"", b"\x00", None, 0):
            with self.subTest(reply=reply), self.linux_flags() as reader:
                reader.return_value = reply
                self.root.chmod(0o775)
                self.assert_resolved_launch(self.link, target)
                self.root.chmod(0o755)
                self.assert_named_launch(self.link, target)

    def test_two_hop_chain_reads_resolved_flags_once(self) -> None:
        """A launch reads the resolved inode once even when two links need trust checks."""
        target = self.same_directory_link()
        second = self.root / "second"
        second.symlink_to(target.name)
        self.link.unlink()
        self.link.symlink_to(second.name)
        with self.linux_flags() as reader:
            self.assertEqual(
                safe_subprocess._checked_executable(str(self.link), (str(target),), ""),
                (str(self.link), target),
            )
        reader.assert_called_once()

    def test_same_directory_writable_modes_keep_named_launch(self) -> None:
        """A writable link directory adds no exposure when it also contains the file."""
        target = self.same_directory_target(self.root)
        self.link.unlink()
        self.link.symlink_to(target.name)
        for mode in (0o775, 0o770, 0o2775, 0o777):
            with self.subTest(mode=oct(mode)):
                self.root.chmod(mode)
                self.assert_named_launch(self.link, target)

    def test_two_hop_same_directory_target_keeps_named_launch(self) -> None:
        """An owned protected first hop can reach a group-writable final directory."""
        nested = self.root / "framework-bin"
        nested.mkdir()
        nested.chmod(0o775)
        target = self.same_directory_target(nested)
        second = nested / "python"
        second.symlink_to(target.name)
        self.link.unlink()
        self.link.symlink_to(second)
        self.assert_named_launch(self.link, target)

    def test_untrusted_first_hop_with_same_directory_target_launches_resolved_file(self) -> None:
        """The final link's exemption cannot excuse an unprotected earlier hop."""
        nested = self.root / "framework-bin"
        nested.mkdir()
        target = self.same_directory_target(nested)
        second = nested / "python"
        second.symlink_to(target.name)
        self.link.unlink()
        self.link.symlink_to(second)
        self.root.chmod(0o777)
        self.assert_resolved_launch(self.link, target)

    def test_same_directory_foreign_link_owner_launches_resolved_file(self) -> None:
        """The directory exemption never exempts link ownership."""
        target = self.same_directory_target(self.root)
        self.link.unlink()
        self.link.symlink_to(target.name)
        original = Path.lstat

        def foreign_owner(path: Path) -> os.stat_result:
            """Plant foreign ownership only on the same-directory link."""
            metadata = original(path)
            if path == self.link:
                fields = list(metadata)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "lstat", foreign_owner):
            self.assert_resolved_launch(self.link, target)

    def test_same_directory_foreign_directory_owner_launches_resolved_file(self) -> None:
        """The directory exemption never exempts directory ownership."""
        target = self.same_directory_target(self.root)
        self.link.unlink()
        self.link.symlink_to(target.name)
        original = Path.stat

        def foreign_owner(path: Path, **kwargs: Any) -> os.stat_result:
            """Plant foreign ownership on the shared containing directory."""
            metadata = original(path, **kwargs)
            if path == self.root:
                fields = list(metadata)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "stat", foreign_owner):
            self.assert_resolved_launch(self.link, target)

    def test_darwin_same_directory_skips_acl_inspection(self) -> None:
        """ACL grants on the resolved file's directory add no link-specific exposure."""
        target = self.same_directory_target(self.root)
        self.link.unlink()
        self.link.symlink_to(target.name)
        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.object(safe_subprocess, "_darwin_entry_protected", return_value=False),
            mock.patch.object(
                safe_subprocess, "_darwin_acl_allows_entries", return_value=True
            ) as acl,
        ):
            self.assert_named_launch(self.link, target)
        acl.assert_not_called()

    def test_resolved_parent_stat_failure_launches_resolved_file(self) -> None:
        """Unreadable resolved-parent metadata cannot authorize the exemption."""
        original = Path.stat

        def unreadable_parent(path: Path, **kwargs: Any) -> os.stat_result:
            """Fail only the resolved file's parent, leaving file validation intact."""
            if path == Path(PYTHON).parent:
                raise PermissionError("planted resolved-parent stat failure")
            return original(path, **kwargs)

        with mock.patch.object(Path, "stat", unreadable_parent):
            self.assert_resolved_launch(self.link)

    def test_same_inode_on_another_device_launches_resolved_file(self) -> None:
        """Matching inode numbers on different devices do not identify one directory."""
        self.root.chmod(0o775)
        resolved_directory = Path(PYTHON).parent.stat()
        original = Path.stat

        def another_device(path: Path, **kwargs: Any) -> os.stat_result:
            """Give the link directory the target inode number on a different device."""
            metadata = original(path, **kwargs)
            if path == self.root:
                fields = list(metadata)
                fields[1] = resolved_directory.st_ino
                fields[2] = resolved_directory.st_dev + 1
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "stat", another_device):
            self.assert_resolved_launch(self.link)

    def test_directory_identity_ignores_path_spelling(self) -> None:
        """Different path spellings can still identify the resolved file's directory."""
        self.root.chmod(0o775)
        resolved_directory = Path(PYTHON).parent.stat()
        original = Path.stat

        def same_identity(path: Path, **kwargs: Any) -> os.stat_result:
            """Model an alias by giving the link directory the resolved directory identity."""
            metadata = original(path, **kwargs)
            if path == self.root:
                fields = list(metadata)
                fields[1] = resolved_directory.st_ino
                fields[2] = resolved_directory.st_dev
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "stat", same_identity):
            self.assert_named_launch(self.link, Path(PYTHON))

    def test_sticky_world_writable_link_directory_is_accepted(self) -> None:
        """Sticky directories protect an owned link despite world write permission."""
        self.root.chmod(0o1777)
        result = run([str(self.link), "-V"], allowed_executables=ALLOW_PYTHON)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.argv[0], str(self.link))

    def test_nonsticky_world_writable_link_directory_launches_resolved_file(self) -> None:
        """Hosted tool-cache permissions select resolved launch and retain caller argv[0]."""
        self.root.chmod(0o777)
        self.assert_resolved_launch(self.link)

    def test_nonsticky_world_writable_path_link_launches_resolved_file(self) -> None:
        """Bare-name lookup also avoids writable links while retaining the bare argv[0]."""
        self.root.chmod(0o777)
        self.assertEqual(
            safe_subprocess._checked_executable(self.link.name, (self.link.name,), str(self.root)),
            (PYTHON, Path(PYTHON)),
        )
        result = run(
            [self.link.name, "-c", "import sys; print(sys.orig_argv[0])"],
            allowed_executables=(self.link.name,),
            env={"PATH": str(self.root)},
            capture_output=True,
            text=True,
        )
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.argv[0], PYTHON)
        self.assertEqual(result.stdout.strip(), self.link.name)

    def test_group_writable_link_directory_launches_resolved_file(self) -> None:
        """Group write is untrusted regardless of membership, including setgid directories."""
        self.assertFalse(hasattr(safe_subprocess, "grp"))
        self.assertFalse(hasattr(safe_subprocess, "pwd"))
        for mode in (0o775, 0o770, 0o2775):
            with self.subTest(mode=oct(mode)):
                self.root.chmod(mode)
                self.assert_resolved_launch(self.link)

    def test_other_posix_platform_launches_resolved_file(self) -> None:
        """Uninspected ACL models cannot authorize a named launch through trusted modes."""
        with mock.patch.object(sys, "platform", "freebsd14"):
            self.assert_resolved_launch(self.link)

    def test_linux_does_not_inspect_darwin_acls(self) -> None:
        """Linux mode bits cover the POSIX ACL mask without loading the macOS interface."""
        with (
            mock.patch.object(sys, "platform", "linux"),
            mock.patch.object(safe_subprocess, "_darwin_acl_allows_entries") as acl,
        ):
            self.assertEqual(
                safe_subprocess._checked_executable(str(self.link), ALLOW_PYTHON, ""),
                (str(self.link), Path(PYTHON)),
            )
        acl.assert_not_called()

    def test_darwin_checks_each_link_directory(self) -> None:
        """Each followed link's parent receives one ACL check per validation."""
        nested = self.root / "nested"
        nested.mkdir(mode=0o755)
        second = nested / "second"
        second.symlink_to(PYTHON)
        self.link.unlink()
        self.link.symlink_to("nested/second")
        with (
            mock.patch.object(sys, "platform", "darwin"),
            mock.patch.object(
                safe_subprocess, "_darwin_acl_allows_entries", return_value=False
            ) as acl,
        ):
            self.assertEqual(
                safe_subprocess._checked_executable(str(self.link), ALLOW_PYTHON, ""),
                (str(self.link), Path(PYTHON)),
            )
        self.assertEqual(acl.call_args_list, [mock.call(self.root), mock.call(nested)])

    def test_darwin_acl_grant_launches_resolved_file(self) -> None:
        """An ACL grant overrides trusted modes and sticky directory protection."""
        for mode in (0o755, 0o1777):
            with self.subTest(mode=oct(mode)):
                self.root.chmod(mode)
                with (
                    mock.patch.object(sys, "platform", "darwin"),
                    mock.patch.object(
                        safe_subprocess, "_darwin_acl_allows_entries", return_value=True
                    ) as acl,
                ):
                    self.assert_resolved_launch(self.link)
                self.assertEqual(acl.call_args_list, [mock.call(self.root)] * 2)

    def test_second_link_in_untrusted_directory_launches_resolved_file(self) -> None:
        """Every hop needs protection, including a relative target in another directory."""
        unsafe = self.root / "unsafe"
        unsafe.mkdir()
        unsafe.chmod(0o777)
        second = unsafe / "second"
        second.symlink_to(PYTHON)
        self.link.unlink()
        self.link.symlink_to("unsafe/second")
        self.assert_resolved_launch(self.link)

    def test_foreign_link_owner_launches_resolved_file(self) -> None:
        """A foreign-owned link selects the resolved file even inside an owned directory."""
        original = Path.lstat

        def foreign_owner(path: Path) -> os.stat_result:
            """Plant foreign ownership only on the fixture link."""
            metadata = original(path)
            if path == self.link:
                fields = list(metadata)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "lstat", foreign_owner):
            self.assert_resolved_launch(self.link)

    def test_foreign_directory_owner_launches_resolved_file(self) -> None:
        """A third-user directory owner can replace links even in sticky directories."""
        self.root.chmod(0o1777)
        original = Path.stat

        def foreign_owner(path: Path, **kwargs: Any) -> os.stat_result:
            """Plant foreign ownership only on the fixture directory."""
            metadata = original(path, **kwargs)
            if path == self.root:
                fields = list(metadata)
                fields[4] = os.getuid() + 1
                return os.stat_result(fields)
            return metadata

        with mock.patch.object(Path, "stat", foreign_owner):
            self.assert_resolved_launch(self.link)

    def test_link_metadata_lookup_failure_is_untrusted(self) -> None:
        """Link and directory metadata errors cannot authorize named-path execution."""
        for method in ("lstat", "stat"):
            with self.subTest(method=method):
                with mock.patch.object(Path, method, side_effect=PermissionError("planted")):
                    self.assertFalse(
                        safe_subprocess._executable_links_trusted((self.link,), Path(PYTHON))
                    )

    def test_link_walk_follows_targets_before_dotdot(self) -> None:
        """Directory links, absolute targets, dots and dotdots retain kernel order."""
        nested = self.root / "nested"
        nested.mkdir()
        directory = self.root / "directory"
        directory.symlink_to(nested, target_is_directory=True)
        self.link.unlink()
        self.link.symlink_to("./directory/../last")
        last = self.root / "last"
        last.symlink_to(PYTHON)
        self.assertEqual(safe_subprocess._executable_links(self.link), (self.link, directory, last))
        self.assertEqual(
            run([str(self.link), "-V"], allowed_executables=ALLOW_PYTHON).returncode, 0
        )

    def test_link_chain_40_accepted_and_41_refused(self) -> None:
        """The named hop bound admits forty links and rejects the forty-first."""
        for number in range(1, safe_subprocess.MAX_EXECUTABLE_LINK_HOPS):
            link = self.root / f"hop-{number}"
            link.symlink_to(self.link.name)
            self.link = link
        self.assertEqual(len(safe_subprocess._executable_links(self.link)), 40)
        # Some POSIX kernels permit fewer hops; validation must still accept forty.
        launch, resolved = safe_subprocess._checked_executable(str(self.link), ALLOW_PYTHON, "")
        self.assertEqual(launch, str(self.link))
        self.assertEqual(str(resolved), PYTHON)
        if os.access(self.link, os.X_OK):
            self.assertEqual(
                run([str(self.link), "-V"], allowed_executables=ALLOW_PYTHON).returncode, 0
            )
        extra = self.root / "hop-40"
        extra.symlink_to(self.link.name)
        with self.assertRaisesRegex(CommandValidationError, "MAX_EXECUTABLE_LINK_HOPS=40"):
            run([str(extra), "-V"], allowed_executables=ALLOW_PYTHON)

    def test_link_cycle_reaches_named_hop_bound(self) -> None:
        """A cycle stops at the hop bound instead of looping forever."""
        self.link.unlink()
        self.link.symlink_to(self.link.name)
        with self.assertRaisesRegex(CommandValidationError, "MAX_EXECUTABLE_LINK_HOPS=40"):
            run([str(self.link)], allowed_executables=ALLOW_PYTHON)

    def test_walk_step_bound_is_enforced(self) -> None:
        """The total step limit fails closed independently of the link limit."""
        with mock.patch.object(safe_subprocess, "MAX_EXECUTABLE_WALK_STEPS", 1):
            with self.assertRaisesRegex(CommandValidationError, "MAX_EXECUTABLE_WALK_STEPS=1"):
                run([str(self.link), "-V"], allowed_executables=ALLOW_PYTHON)

    def test_walk_exact_step_bound_is_accepted(self) -> None:
        """Finishing on the last permitted component is accepted."""
        with mock.patch.object(
            safe_subprocess, "MAX_EXECUTABLE_WALK_STEPS", len(Path(PYTHON).parts)
        ):
            self.assertEqual(safe_subprocess._executable_links(Path(PYTHON)), ())

    def test_link_chain_does_not_bypass_allowlist(self) -> None:
        """Trusted ownership grants no permission to execute an unallowlisted file."""
        other = self.root / "other"
        other.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        other.chmod(0o755)
        self.link.unlink()
        self.link.symlink_to(other.name)
        chain = self.root / "chain"
        chain.symlink_to(self.link.name)
        with self.assertRaisesRegex(CommandValidationError, "executable is not allowlisted"):
            run([str(chain)], allowed_executables=ALLOW_PYTHON)

    def test_without_getuid_launches_resolved_file(self) -> None:
        """The Windows ownership fallback launches the checked file, not a link."""
        with mock.patch.object(os, "getuid", create=True):
            del os.getuid
            self.assertEqual(
                safe_subprocess._checked_executable(str(self.link), ALLOW_PYTHON, ""),
                (PYTHON, Path(PYTHON)),
            )


if __name__ == "__main__":
    unittest.main()
