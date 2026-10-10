#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""scripts.lib.scratch_program waits out a held program file, and only that.

The Windows failure is planted with a mock: an ``os.unlink`` that raises
``PermissionError`` for the program a given number of times, as Windows does
while Defender still holds a program that has just run.
"""

from __future__ import annotations

import contextlib
import os
import shutil
import sys
import tempfile
import unittest
from collections.abc import Callable
from pathlib import Path
from typing import Any
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.scratch_program import ATTEMPTS, remove_program

REAL_UNLINK = os.unlink


def held_for(name: str, times: int) -> tuple[Callable[..., None], list[str]]:
    """An unlink that refuses files called ``name`` ``times`` times, then deletes."""
    calls: list[str] = []

    def unlink(path: str | os.PathLike[str], *args: Any, **kwargs: Any) -> None:
        base = Path(os.fspath(path)).name
        calls.append(base)
        if base == name and calls.count(base) <= times:
            raise PermissionError(13, "Access is denied", base)
        REAL_UNLINK(path, *args, **kwargs)

    return unlink, calls


class RemoveProgram(unittest.TestCase):
    def setUp(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        self.addCleanup(scratch.cleanup)
        self.dir = Path(scratch.name)

    def program(self, name: str = "harness") -> Path:
        path = self.dir / name
        path.write_bytes(b"MZ")
        return path

    def test_a_program_held_a_few_times_is_removed(self) -> None:
        path = self.program()
        unlink, calls = held_for("harness", 3)
        remove_program(path, delay=0, unlink=unlink)
        self.assertFalse(path.exists())
        self.assertEqual(calls, ["harness"] * 4)

    def test_a_program_held_longer_than_the_bound_raises(self) -> None:
        path = self.program()
        unlink, calls = held_for("harness", ATTEMPTS)
        with self.assertRaises(PermissionError):
            remove_program(path, delay=0, unlink=unlink)
        self.assertEqual(len(calls), ATTEMPTS)
        self.assertTrue(path.exists())

    def test_another_error_is_not_retried(self) -> None:
        path = self.program()
        refuse = mock.Mock(side_effect=OSError(30, "Read-only file system"))
        with self.assertRaises(OSError):
            remove_program(path, delay=0, unlink=refuse)
        self.assertEqual(refuse.call_count, 1)

    def test_the_windows_spelling_is_removed(self) -> None:
        path = self.program("harness.exe")
        remove_program(self.dir / "harness", delay=0)
        self.assertFalse(path.exists())

    def test_a_missing_program_is_nothing_to_do(self) -> None:
        refuse = mock.Mock(side_effect=AssertionError("unlink called"))
        remove_program(self.dir / "never-built", delay=0, unlink=refuse)
        refuse.assert_not_called()

    def test_a_bound_below_one_is_refused(self) -> None:
        with self.assertRaises(ValueError):
            remove_program(self.program(), attempts=0)


def windows_rmtree() -> contextlib.AbstractContextManager[object]:
    """Make ``shutil.rmtree`` take the path it takes on Windows.

    On Linux ``rmtree`` walks with directory file descriptors, and
    ``tempfile``'s error handler then retries a refused file until it goes.
    Windows has no such functions: ``rmtree`` unlinks by path, and the
    handler tries once more, then raises. That is the CI failure.
    """
    internals = vars(shutil)  # private names typeshed does not declare
    if "_rmtree_impl" in internals:  # Python 3.14
        return mock.patch.object(shutil, "_rmtree_impl", internals["_rmtree_unsafe"])
    return mock.patch.object(shutil, "_use_fd_functions", False)


class TemporaryDirectoryCleanup(unittest.TestCase):
    """The CI failure, reproduced on any host, and the order that avoids it."""

    def test_cleanup_alone_fails_on_a_held_program(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        (Path(scratch.name) / "harness").write_bytes(b"MZ")
        unlink, _ = held_for("harness", 2)
        with mock.patch("os.unlink", unlink), windows_rmtree():
            with self.assertRaises(PermissionError):
                scratch.cleanup()
        scratch.cleanup()

    def test_removing_the_program_first_lets_cleanup_finish(self) -> None:
        scratch = tempfile.TemporaryDirectory()
        (Path(scratch.name) / "harness").write_bytes(b"MZ")
        (Path(scratch.name) / "main.cpp").write_text("int main() {}\n")
        unlink, calls = held_for("harness", 2)
        with mock.patch("os.unlink", unlink), windows_rmtree():
            remove_program(Path(scratch.name) / "harness", delay=0)
            scratch.cleanup()
        self.assertFalse(Path(scratch.name).exists())
        self.assertEqual(calls.count("harness"), 3)


if __name__ == "__main__":
    unittest.main()
