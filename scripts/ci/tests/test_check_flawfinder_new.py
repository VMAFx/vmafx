#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Planted-defect recall for scripts/ci/check_flawfinder_new.py.

Each case builds a throwaway git repository with a base commit on `master`,
changes a C file on a branch and runs the check against `master`:

- an added `fopen()` (the #3216 Codacy finding) is reported, exit 1;
- a finding the base already carries passes when its line moves;
- reading an input through `fdopen()` on a descriptor passes;
- a new file with a finding is reported;
- a missing flawfinder is a tool error, exit 2, never a pass.
"""

from __future__ import annotations

import contextlib
import io
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_flawfinder_new as check
from scripts.lib.safe_subprocess import run as run_command

BASE = """\
#include <stdio.h>

int main(int argc, char **argv)
{
    FILE *f = fopen(argv[1], "rb");
    return f == NULL || argc < 2;
}
"""
MOVED = "/* moved down */\n\n" + BASE
ADDED = BASE.replace(
    "    return f == NULL",
    '    FILE *g = fopen(argv[2], "rb");\n    return g == NULL || f == NULL',
)
FDOPEN = BASE.replace(
    "    return f == NULL",
    '    FILE *g = fdopen(3, "rb");\n    return g == NULL || f == NULL',
)


class CheckFlawfinderNew(unittest.TestCase):
    def setUp(self) -> None:
        # No skip: the pre-commit hook installs flawfinder, and a missing one
        # fails here instead of passing silently.
        self.assertIsNotNone(shutil.which("flawfinder"), "flawfinder is not on PATH")
        temp = tempfile.TemporaryDirectory(prefix="vmafx-flawfinder-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.git("init", "-q", "-b", "master")
        (self.root / "harness.c").write_text(BASE, encoding="utf-8")
        self.git("add", "harness.c")
        self.git("commit", "-q", "-m", "base")
        self.git("switch", "-q", "-c", "change")

    def git(self, *args: str) -> None:
        git = shutil.which("git")
        assert git is not None
        # Drop the caller's repository variables (a hook runs with GIT_DIR and
        # GIT_INDEX_FILE set) so the fixture never touches the caller's repository.
        env = {
            **{k: v for k, v in os.environ.items() if not k.startswith("GIT_")},
            "GIT_CONFIG_GLOBAL": os.devnull,
            "GIT_CONFIG_SYSTEM": os.devnull,
            "GIT_AUTHOR_NAME": "t",
            "GIT_AUTHOR_EMAIL": "t@example.invalid",
            "GIT_COMMITTER_NAME": "t",
            "GIT_COMMITTER_EMAIL": "t@example.invalid",
        }
        proc = run_command(
            [git, "-c", "commit.gpgsign=false", *args],
            allowed_executables=(git,),
            cwd=self.root,
            env=env,
            capture_output=True,
            text=True,
            timeout_seconds=60,
        )
        self.assertEqual(proc.returncode, 0, proc.stderr)

    def run_check(self, name: str, text: str) -> tuple[int, str]:
        (self.root / name).write_text(text, encoding="utf-8")
        out = io.StringIO()
        cwd = Path.cwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                status = check.main(["--base-branch", "master", name])
        finally:
            os.chdir(cwd)
        return status, out.getvalue()

    def test_added_fopen_is_reported(self) -> None:
        status, out = self.run_check("harness.c", ADDED)
        self.assertEqual(status, 1)
        self.assertIn("harness.c:6: flawfinder fopen", out)
        self.assertNotIn("harness.c:5:", out, "the base finding was reported as new")

    def test_moved_base_finding_passes(self) -> None:
        self.assertEqual(self.run_check("harness.c", MOVED), (0, ""))

    def test_fdopen_on_descriptor_passes(self) -> None:
        self.assertEqual(self.run_check("harness.c", FDOPEN), (0, ""))

    def test_new_file_finding_is_reported(self) -> None:
        status, out = self.run_check("fresh.c", BASE)
        self.assertEqual(status, 1)
        self.assertIn("fresh.c:5: flawfinder fopen", out)


class CheckFlawfinderMissing(unittest.TestCase):
    def test_missing_flawfinder_is_a_tool_error(self) -> None:
        with (
            mock.patch("shutil.which", return_value=None),
            contextlib.redirect_stderr(io.StringIO()) as err,
        ):
            self.assertEqual(check.main(["x.c"]), 2)
        self.assertIn("flawfinder is not on PATH", err.getvalue())


if __name__ == "__main__":
    unittest.main()
