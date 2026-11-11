#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Offline planted-defect recall for the Codacy Bandit finding gate."""

from __future__ import annotations

import contextlib
import io
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.ci import check_bandit_new as check
from scripts.lib.safe_subprocess import run as run_command

BASE = "from pathlib import Path\n\nexe = Path('tool')\n"
UNSAFE = "import subprocess\n\nsubprocess.run([exe], check=False, timeout=5)  # noqa: S603\n"
SAFE = "from scripts.lib.safe_subprocess import run\n\nrun([exe], allowed_executables=(exe,))\n"


class CheckBanditNew(unittest.TestCase):
    def setUp(self) -> None:
        self.assertIsNotNone(shutil.which("bandit"), "bandit is not on PATH")
        temp = tempfile.TemporaryDirectory(prefix="vmafx-bandit-")
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.git("init", "-q", "-b", "master")
        (self.root / "harness.py").write_text(BASE, encoding="utf-8")
        self.git("add", "harness.py")
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

    def run_check(self, name: str, source: str) -> tuple[int, str, str]:
        (self.root / name).write_text(source, encoding="utf-8")
        out, err = io.StringIO(), io.StringIO()
        cwd = Path.cwd()
        os.chdir(self.root)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
                status = check.main(["--base-branch", "master", name])
        finally:
            os.chdir(cwd)
        return status, out.getvalue(), err.getvalue()

    def test_noqa_s603_is_new(self) -> None:
        status, out, err = self.run_check("harness.py", BASE + UNSAFE)
        self.assertEqual(status, 1, err)
        self.assertIn("harness.py:6: B603:", out)
        self.assertIn("scripts/lib/safe_subprocess.run", err)

    def test_moved_base_finding_is_old(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.assertEqual(self.run_check("harness.py", "\n" + BASE + UNSAFE)[0], 0)

    def test_renamed_base_finding_is_old(self) -> None:
        (self.root / "harness.py").write_text(BASE + UNSAFE, encoding="utf-8")
        self.git("add", "harness.py")
        self.git("commit", "-q", "-m", "unsafe base")
        self.git("switch", "-q", "master")
        self.git("merge", "--ff-only", "change")
        self.git("switch", "-q", "change")
        self.git("mv", "harness.py", "renamed.py")
        self.assertEqual(self.run_check("renamed.py", BASE + UNSAFE)[0], 0)

    def test_safe_subprocess_passes(self) -> None:
        self.assertEqual(self.run_check("harness.py", BASE + SAFE)[0], 0)

    def test_disabled_b101_passes(self) -> None:
        self.assertEqual(self.run_check("harness.py", BASE + "assert exe\n")[0], 0)

    def test_missing_bandit_is_error(self) -> None:
        with mock.patch.object(check, "tool", side_effect=check.ToolError("bandit missing")):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("bandit missing", err)

    def test_bandit_json_errors_are_error(self) -> None:
        result = mock.Mock(returncode=0, stdout=json.dumps({"results": [], "errors": ["bad"]}))
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("scan errors", err)

    def test_invalid_json_is_error(self) -> None:
        result = mock.Mock(returncode=0, stdout="not JSON")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("Bandit failed", err)

    def test_unexpected_bandit_exit_is_error(self) -> None:
        result = mock.Mock(returncode=3, stdout="", stderr="scan failed")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE)
        self.assertEqual(status, 2)
        self.assertIn("exited 3", err)


if __name__ == "__main__":
    unittest.main()
