#!/usr/bin/env python3
# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Controls for the GIT_* environment builders and their shell twins."""

from __future__ import annotations

import os
import re
import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.git_env import FIXTURE_EMAIL, FIXTURE_NAME, drop, hermetic

ROOT = Path(__file__).resolve().parents[2]
SHELL_CLEAN = ROOT / "scripts/lib/clean-git-env.sh"

EXPORT = re.compile(r"(?m)^export (GIT_[A-Z0-9_]+)=(\S+)\s*$")


def strip_quotes(parsed: dict[str, str]) -> dict[str, str]:
    """Drop the quoting the shell file wraps its values in."""
    return {key: value.strip('"') for key, value in parsed.items()}


class DropTest(unittest.TestCase):
    def test_drop_removes_every_git_variable_and_keeps_the_rest(self) -> None:
        source = {
            "GIT_DIR": "/caller/.git",
            "GIT_INDEX_FILE": "/caller/.git/index",
            "GIT_CONFIG_COUNT": "9",
            "PATH": "/bin",
            "LANG": "C.UTF-8",
        }
        self.assertEqual(drop(source), {"PATH": "/bin", "LANG": "C.UTF-8"})

    def test_drop_defaults_to_the_process_environment_without_mutating_it(self) -> None:
        os.environ["GIT_DIR_TEST_PROBE"] = "/probe"
        try:
            self.assertNotIn("GIT_DIR_TEST_PROBE", drop())
            self.assertEqual(os.environ.get("GIT_DIR_TEST_PROBE"), "/probe")
        finally:
            del os.environ["GIT_DIR_TEST_PROBE"]

    def test_drop_never_mutates_its_input(self) -> None:
        source = {"GIT_DIR": "/x", "HOME": "/h"}
        drop(source)
        self.assertEqual(source, {"GIT_DIR": "/x", "HOME": "/h"})


class HermeticTest(unittest.TestCase):
    def test_hermetic_drops_the_caller_state_and_supplies_the_fixture_identity(self) -> None:
        built = hermetic(
            {
                "GIT_DIR": "/caller/.git",
                "GIT_AUTHOR_EMAIL": "caller@example.invalid",
                "PATH": "/bin",
            }
        )
        self.assertNotIn("GIT_DIR", built)
        self.assertEqual(built["PATH"], "/bin")
        self.assertEqual(built["GIT_AUTHOR_EMAIL"], FIXTURE_EMAIL)
        self.assertEqual(built["GIT_COMMITTER_NAME"], FIXTURE_NAME)
        self.assertEqual(built["GIT_CONFIG_GLOBAL"], os.devnull)
        self.assertEqual(built["GIT_CONFIG_NOSYSTEM"], "1")
        self.assertEqual(built["GIT_CONFIG_COUNT"], "3")
        self.assertEqual(built["GIT_CONFIG_VALUE_1"], "false")
        self.assertEqual(built["GIT_CONFIG_VALUE_2"], "0")

    def test_hermetic_never_mutates_its_input(self) -> None:
        source = {"GIT_DIR": "/x"}
        hermetic(source)
        self.assertEqual(source, {"GIT_DIR": "/x"})


class ShellTwinDriftTest(unittest.TestCase):
    """The Python hermetic() must export exactly what clean-git-env.sh exports."""

    def test_hermetic_matches_the_shell_exports(self) -> None:
        shell = strip_quotes(dict(EXPORT.findall(SHELL_CLEAN.read_text(encoding="utf-8"))))
        self.assertTrue(shell, "the export lines moved: fix this test's parser")
        built = hermetic({})
        self.assertEqual(
            set(built) - {"PATH"},
            set(shell),
            "hermetic() and clean-git-env.sh export different GIT_* sets",
        )
        for key, value in shell.items():
            # The shell file spells the null device /dev/null (bash runs under MSYS);
            # git accepts the platform device equally, so the twin uses os.devnull.
            expected = os.devnull if value == "/dev/null" else value
            self.assertEqual(built[key], expected, key)

    def test_the_shell_fixture_identity_matches_the_python_constants(self) -> None:
        shell = strip_quotes(dict(EXPORT.findall(SHELL_CLEAN.read_text(encoding="utf-8"))))
        self.assertEqual(shell["GIT_AUTHOR_NAME"], FIXTURE_NAME)
        self.assertEqual(shell["GIT_AUTHOR_EMAIL"], FIXTURE_EMAIL)


if __name__ == "__main__":
    unittest.main()
