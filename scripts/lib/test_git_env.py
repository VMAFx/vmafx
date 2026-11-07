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

from scripts.lib.git_env import (
    FIXTURE_EMAIL,
    FIXTURE_NAME,
    drop,
    drop_except,
    hermetic,
)

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


class DropExceptTest(unittest.TestCase):
    """The whitelist twin: scrub the namespace, re-admit the settings."""

    def test_drop_except_re_admits_only_the_keep_prefixes(self) -> None:
        source = {
            "GIT_DIR": "/caller/.git",
            "GIT_INDEX_FILE": "/caller/.git/index",
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "user.name",
            "GIT_AUTHOR_NAME": "a",
            "PATH": "/bin",
        }
        self.assertEqual(
            drop_except(source, prefixes=("GIT_",), keep=("GIT_CONFIG_", "GIT_AUTHOR_")),
            {
                "GIT_CONFIG_COUNT": "1",
                "GIT_CONFIG_KEY_0": "user.name",
                "GIT_AUTHOR_NAME": "a",
                "PATH": "/bin",
            },
        )

    def test_drop_except_with_empty_keep_equals_drop(self) -> None:
        source = {"GIT_DIR": "/x", "GIT_CONFIG_COUNT": "1", "HOME": "/h"}
        self.assertEqual(drop_except(source), drop(source))
        self.assertEqual(drop_except(source, prefixes=("GIT_",), keep=()), drop(source))

    def test_drop_except_reads_each_key_once_and_never_mutates_its_input(self) -> None:
        source = {"GIT_DIR": "/x", "GIT_CONFIG_KEY_0": "k", "HOME": "/h"}
        snapshot = dict(source)
        drop_except(source, keep=("GIT_CONFIG_",))
        self.assertEqual(source, snapshot)

    def test_drop_except_defaults_to_the_process_environment(self) -> None:
        os.environ["GIT_DIR_TEST_PROBE"] = "/probe"
        os.environ["GIT_CONFIG_TEST_PROBE"] = "k"
        try:
            built = drop_except(keep=("GIT_CONFIG_",))
            self.assertNotIn("GIT_DIR_TEST_PROBE", built)
            self.assertEqual(built.get("GIT_CONFIG_TEST_PROBE"), "k")
        finally:
            del os.environ["GIT_DIR_TEST_PROBE"]
            del os.environ["GIT_CONFIG_TEST_PROBE"]


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
