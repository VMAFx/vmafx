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
from scripts.lib import new_scanner_findings as shared
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
        self.assertIn("fix each finding", err)
        self.assertIn("scripts.lib.safe_subprocess.run", err)
        self.assertIn("# noqa does not silence Bandit", err)

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
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("scan errors", err)

    def test_invalid_json_is_error(self) -> None:
        result = mock.Mock(returncode=0, stdout="not JSON")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("Bandit failed", err)

    def test_unexpected_bandit_exit_is_error(self) -> None:
        result = mock.Mock(returncode=3, stdout="", stderr="scan failed")
        with mock.patch.object(check, "run_command", return_value=result):
            status, _, err = self.run_check("harness.py", BASE + "# changed\n")
        self.assertEqual(status, 2)
        self.assertIn("exited 3", err)


class CheckSharedScannerFindings(unittest.TestCase):
    def test_git_output_drops_injected_git_configuration(self) -> None:
        injected = {
            "GIT_CONFIG_COUNT": "1",
            "GIT_CONFIG_KEY_0": "diff.noprefix",
            "GIT_CONFIG_VALUE_0": "true",
            "GIT_DIR": "/wrong/repository",
            "SAFE_ENV": "kept",
        }
        result = mock.Mock(returncode=0, stdout="repository config")
        with (
            mock.patch.object(shared, "os", mock.Mock(environ=injected)),
            mock.patch.object(shared, "run_command", return_value=result) as run,
        ):
            self.assertEqual(
                shared.git_output("git", ["config", "--get", "diff.noprefix"], Path.cwd()),
                (0, "repository config"),
            )
        self.assertEqual(run.call_args.kwargs["env"], {"SAFE_ENV": "kept"})

    def test_identical_text_skips_both_scans(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "same.py"
            current.write_text("one line\n", encoding="utf-8")
            scan = mock.Mock(side_effect=AssertionError("scan called"))
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        scan.assert_not_called()

    def test_one_changed_byte_scans_both_copies(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "changed.py"
            current.write_text("one line!\n", encoding="utf-8")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, "one line\n"), [])
        self.assertEqual(scan.call_count, 2)

    def test_non_utf8_file_scans_both_copies(self) -> None:
        with tempfile.TemporaryDirectory(prefix="vmafx-scanner-") as temp:
            current = Path(temp) / "latin1.c"
            current.write_bytes(b"/* caf\xe9 */\n")
            scan = mock.Mock(return_value=[])
            self.assertEqual(shared.new_findings(scan, current, ""), [])
        self.assertEqual(scan.call_count, 2)


if __name__ == "__main__":
    unittest.main()
