#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Exercise merge-base ownership and fail-closed pre-push selection with real Git."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("pre-push-mypy.py").resolve()
CONFIG = SCRIPT.parents[2] / ".pre-commit-config.yaml"
GIT = shutil.which("git") or "/usr/bin/git"
PRE_COMMIT = shutil.which("pre-commit") or "/usr/bin/pre-commit"


def hook_config(identifier: str) -> str:
    """Extract one hook for execution with the actual installed framework."""
    return CONFIG.read_text().split(f"      - id: {identifier}\n", 1)[1].split("      - id:", 1)[0]


class MypyScope(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "repo"
        self.root.mkdir()
        self.environment = {
            k: v for k, v in os.environ.items() if not k.startswith(("GIT_", "PRE_COMMIT_"))
        }
        self.environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.git("init", "-q", "--initial-branch=master")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.write("scripts/api.py", "def value() -> int:\n    return 1\n")
        self.write("scripts/debt.py", 'unchanged: int = "debt"\n')
        self.commit("base")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        self.git("switch", "-qc", "feature")
        binary = self.directory / "bin"
        binary.mkdir()
        self.checker = binary / "mypy"
        self.receipt = self.directory / "checked.json"
        self.checker.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            'pathlib.Path(os.environ["CHECKED_PATH"]).write_text(json.dumps(sys.argv[1:]))\n'
            'print("CHECKED", *sys.argv[1:])\n'
            'raise SystemExit(int(os.environ.get("CHECKER_STATUS", "0")))\n'
        )
        self.checker.chmod(0o700)
        self.environment["PATH"] = str(binary) + os.pathsep + self.environment["PATH"]
        self.environment["CHECKED_PATH"] = str(self.receipt)

    def git(self, *args: str) -> str:
        return subprocess.check_output(  # noqa: S603 -- disposable Git fixture
            [GIT, "-C", str(self.root), *args],
            env=self.environment,
            text=True,
            stderr=subprocess.PIPE,
        ).strip()

    def write(self, filename: str, content: str = "owned: int = 1\n") -> Path:
        path = self.root / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-qm", message)

    def run_hook(self, *args: str) -> subprocess.CompletedProcess[str]:
        return subprocess.run(  # noqa: S603 -- shipped hook and disposable fixture
            [sys.executable, str(SCRIPT), *args],
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )

    def assert_checked(self, expected: list[str], status: int = 0) -> None:
        result = self.run_hook()
        self.assertEqual(result.returncode, status, result.stderr)
        self.assertEqual(json.loads(self.receipt.read_text()), expected)

    def test_complete_owned_scope_excludes_master_and_other_packages(self) -> None:
        for filename in (
            "ai/owned.py",
            "scripts/owned.py",
            "tools/other.py",
            "root.py",
            "ai/note.txt",
        ):
            self.write(filename)
        self.commit("owned files")
        self.assert_checked(["ai/owned.py", "scripts/owned.py"])
        self.assertEqual(self.run_hook("ai/owned.py").returncode, 0)
        self.assertEqual(json.loads(self.receipt.read_text()), ["ai/owned.py", "scripts/owned.py"])

    def test_rebase_rechecks_unchanged_owned_file_through_real_pre_commit(self) -> None:
        self.write("scripts/owned.py", "from api import value\nowned: int = value()\n")
        # Use the shipped hook configuration so missing always_run or restored
        # filename intersection is caught by the framework, not just unit calls.
        hook = hook_config("mypy-local")
        self.write("scripts/git-hooks/pre-push-mypy.py", SCRIPT.read_text())
        self.write(
            ".pre-commit-config.yaml",
            "repos:\n  - repo: local\n    hooks:\n      - id: mypy-local\n" + hook,
        )
        self.commit("feature with integer consumer")
        old_tip = self.git("rev-parse", "HEAD")
        self.git("switch", "-q", "master")
        self.write("scripts/api.py", 'def value() -> str:\n    return "changed"\n')
        self.commit("master changes dependency type")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        self.git("switch", "-q", "feature")
        self.git("rebase", "master")
        self.assertEqual(self.git("diff", "--name-only", old_tip, "HEAD"), "scripts/api.py")
        self.assertEqual(self.git("diff", old_tip, "HEAD", "--", "scripts/owned.py"), "")
        self.environment["CHECKER_STATUS"] = "1"
        result = subprocess.run(  # noqa: S603 -- installed framework, disposable Git fixture
            [
                PRE_COMMIT,
                "run",
                "mypy-local",
                "--hook-stage",
                "pre-push",
                "--from-ref",
                old_tip,
                "--to-ref",
                "HEAD",
            ],
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(
            json.loads(self.receipt.read_text()),
            ["scripts/git-hooks/pre-push-mypy.py", "scripts/owned.py"],
        )
        # An empty old-tip/new-tip file list must still recheck the owned set.
        result = subprocess.run(  # noqa: S603 -- installed framework, disposable Git fixture
            [
                PRE_COMMIT,
                "run",
                "mypy-local",
                "--hook-stage",
                "pre-push",
                "--from-ref",
                "HEAD",
                "--to-ref",
                "HEAD",
            ],
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("scripts/owned.py", result.stdout)

    def test_type_change_from_symlink_is_checked(self) -> None:
        link = self.root / "scripts/owned.py"
        link.symlink_to("api.py")
        self.commit("base symlink")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        link.unlink()
        self.write("scripts/owned.py", 'owned: int = "invalid"\n')
        self.commit("replace link with source")
        self.assertEqual(
            self.git("diff", "--name-status", "origin/master", "HEAD"), "T\tscripts/owned.py"
        )
        self.environment["CHECKER_STATUS"] = "1"
        self.assert_checked(["scripts/owned.py"], 1)

    def test_symlink_keeps_lexical_identity(self) -> None:
        (self.root / "scripts/owned.py").symlink_to("debt.py")
        self.commit("new link to unchanged debt")
        self.environment["CHECKER_STATUS"] = "1"
        self.assert_checked(["scripts/owned.py"], 1)

    def test_unsafe_or_missing_symlink_target_fails_before_mypy(self) -> None:
        outside = self.directory / "outside.py"
        outside.write_text("outside: int = 1\n")
        link = self.root / "scripts/owned.py"
        for target in (str(outside), "missing.py", ".", "owned.py"):
            with self.subTest(target=target):
                link.symlink_to(target)
                self.commit("link target")
                result = self.run_hook()
                self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
                self.assertFalse(self.receipt.exists())
                link.unlink()

    def test_renamed_destination_checked_and_deletion_omitted(self) -> None:
        self.git("mv", "scripts/api.py", "scripts/renamed.py")
        self.git("rm", "scripts/debt.py")
        self.commit("rename and delete")
        self.assert_checked(["scripts/renamed.py"])

    def test_missing_selected_file_fails_before_mypy(self) -> None:
        path = self.write("scripts/owned.py")
        self.commit("owned source")
        path.unlink()
        self.assertEqual(self.run_hook().returncode, 2)
        self.assertFalse(self.receipt.exists())

    def test_outgoing_target_must_match_head(self) -> None:
        self.write("scripts/owned.py")
        self.commit("owned source")
        other = self.git("rev-parse", "HEAD")
        self.git("switch", "-q", "master")
        for variable in ("PRE_COMMIT_TO_REF", "PRE_COMMIT_LOCAL_BRANCH"):
            for target in (other, "missing-target", ""):
                with self.subTest(variable=variable, target=target):
                    self.environment[variable] = target
                    self.assertEqual(self.run_hook().returncode, 2)
                    self.assertFalse(self.receipt.exists())
            self.environment.pop(variable)

    def test_same_outgoing_commit_and_mypy_failure_propagate(self) -> None:
        self.write("scripts/owned.py")
        self.commit("owned source")
        self.environment["PRE_COMMIT_TO_REF"] = self.git("rev-parse", "HEAD")
        self.environment["CHECKER_STATUS"] = "7"
        self.assert_checked(["scripts/owned.py"], 7)

    def test_missing_master_or_checker_fails_closed(self) -> None:
        self.git("update-ref", "-d", "refs/remotes/origin/master")
        self.assertEqual(self.run_hook().returncode, 2)
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        self.checker.unlink()
        (self.checker.parent / "git").symlink_to(GIT)
        self.environment["PATH"] = str(self.checker.parent)
        self.assertEqual(self.run_hook().returncode, 2)
        self.assertFalse(self.receipt.exists())

    def test_no_owned_files_is_truthful_noop(self) -> None:
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("no ai/scripts Python files", result.stdout)
        self.assertFalse(self.receipt.exists())

    def test_regression_hook_is_registered_for_local_and_ci_checks(self) -> None:
        hook = hook_config("mypy-scope-contract")
        self.assertIn("entry: python3 scripts/git-hooks/test-pre-push-mypy.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("pass_filenames: false", hook)
        scope = hook_config("mypy-local")
        self.assertIn("always_run: true", scope)
        self.assertIn("pass_filenames: false", scope)
        self.assertNotIn("\n        files:", scope)


if __name__ == "__main__":
    unittest.main()
