#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for scripts/ci/git_clone_retry.py (issue 3392)."""

from __future__ import annotations

import importlib.util
import subprocess
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parents[3]
SPEC = importlib.util.spec_from_file_location(
    "git_clone_retry", ROOT / "scripts/ci/git_clone_retry.py"
)
assert SPEC is not None and SPEC.loader is not None
MODULE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(MODULE)


class GitCloneRetryTests(unittest.TestCase):
    def result(self, status: int) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(["git", "clone"], status)

    def test_second_attempt_returns_success(self) -> None:
        with mock.patch.object(
            MODULE.subprocess, "run", side_effect=[self.result(1), self.result(0)]
        ) as run:
            self.assertEqual(
                MODULE.main(["--attempts=3", "--delay=0", "--", "--depth=1", "url", "dest"]),
                0,
            )
        self.assertEqual(run.call_count, 2)
        self.assertEqual(
            run.call_args_list[0].args[0][1:],
            ["clone", "--depth=1", "url", "dest"],
        )

    def test_all_attempts_fail_with_the_last_status(self) -> None:
        with mock.patch.object(
            MODULE.subprocess, "run", side_effect=[self.result(1), self.result(2), self.result(3)]
        ) as run:
            self.assertEqual(
                MODULE.main(["--attempts=3", "--delay=0", "--", "url", "dest"]),
                3,
            )
        self.assertEqual(run.call_count, 3)

    def test_attempts_are_bounded(self) -> None:
        for value in ("0", "11", "not-a-number"):
            with self.subTest(value=value):
                with self.assertRaises(ValueError):
                    MODULE.parse_args([f"--attempts={value}", "--", "url", "dest"])

    def test_clone_arguments_are_preserved(self) -> None:
        attempts, delay, arguments = MODULE.parse_args(
            ["--attempts", "4", "--delay", "2", "--", "--filter=blob:none", "url", "dest"]
        )
        self.assertEqual((attempts, delay), (4, 2))
        self.assertEqual(arguments, ["--filter=blob:none", "url", "dest"])


if __name__ == "__main__":
    unittest.main()
