#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pin the fail-closed, complete-scope mypy contract with a disposable Git tree."""

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
ROOT = SCRIPT.parents[2]
CONFIG = ROOT / ".pre-commit-config.yaml"
MAKEFILE = ROOT / "Makefile"
WORKFLOW = ROOT / ".github/workflows/lint-and-format.yml"
MYPY_CONFIG = ROOT / "pyproject.toml"
GIT = shutil.which("git") or "/usr/bin/git"


def hook_config(identifier: str) -> str:
    """Extract one local hook from the shipped pre-commit configuration."""
    return CONFIG.read_text().split(f"      - id: {identifier}\n", 1)[1].split("      - id:", 1)[0]


class MypyScope(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.root = self.directory / "repo"
        self.root.mkdir()
        self.environment = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("GIT_", "PRE_COMMIT_", "MYPY_"))
        }
        self.environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        self.git("init", "-q", "--initial-branch=master")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.write("ai/src/vmaf_train/model.py")
        self.write("ai/scripts/train.py")
        self.write("scripts/check.py")
        self.write("tools/outside.py", "outside: int = 1  # BAD\n")
        self.write("scripts/note.txt", "BAD\n")
        self.commit("fixture")

        binary = self.directory / "bin"
        binary.mkdir()
        self.checker = binary / "mypy"
        self.checker_python = binary / "python"
        self.checker_python.symlink_to(sys.executable)
        self.calls = self.directory / "calls.jsonl"
        self.checker.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            "with open(os.environ['MYPY_CALLS'], 'a') as handle:\n"
            "    handle.write(json.dumps(sys.argv[1:]) + chr(10))\n"
            "found = 0\n"
            "for name in (a for a in sys.argv[1:] if not a.startswith('--')):\n"
            "    for number, line in enumerate(pathlib.Path(name).read_text().splitlines(), 1):\n"
            "        if 'BAD' in line:\n"
            "            found += 1\n"
            "            print(f'{name}:{number}: error: planted finding  [assignment]')\n"
            "status = int(os.environ.get('MYPY_STATUS', '0'))\n"
            "raise SystemExit(status or bool(found))\n"
        )
        self.checker.chmod(0o700)
        self.environment["PATH"] = str(binary) + os.pathsep + self.environment["PATH"]
        self.environment["MYPY_CALLS"] = str(self.calls)

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

    def calls_checked(self) -> list[list[str]]:
        return [json.loads(line) for line in self.calls.read_text().splitlines()]

    def test_every_tracked_owned_python_file_is_checked_on_every_run(self) -> None:
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        checked = {
            argument
            for call in self.calls_checked()
            for argument in call
            if not argument.startswith("--")
        }
        self.assertEqual(
            checked,
            {"ai/scripts/train.py", "ai/src/vmaf_train/model.py", "scripts/check.py"},
        )

    def test_inherited_finding_blocks_without_a_baseline_exemption(self) -> None:
        self.write("scripts/check.py", 'owned: int = "debt"  # BAD\n')
        self.commit("existing finding")
        result = self.run_hook()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("scripts/check.py", result.stdout + result.stderr)

    def test_each_import_root_has_an_explicit_package_base_invocation(self) -> None:
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        calls = self.calls_checked()
        self.assertEqual(len(calls), 3)
        self.assertTrue(all("--explicit-package-bases" in call for call in calls))
        self.assertTrue(all(f"--python-executable={self.checker_python}" in call for call in calls))
        self.assertEqual(
            [[argument for argument in call if not argument.startswith("--")] for call in calls],
            [
                ["ai/src/vmaf_train/model.py"],
                ["ai/scripts/train.py"],
                ["scripts/check.py"],
            ],
        )

    def test_checker_failure_without_diagnostics_blocks(self) -> None:
        self.environment["MYPY_STATUS"] = "7"
        result = self.run_hook()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_missing_checker_blocks(self) -> None:
        self.checker.unlink()
        (self.checker.parent / "git").symlink_to(GIT)
        self.environment["PATH"] = str(self.checker.parent)
        result = self.run_hook()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("mypy is required", result.stderr)

    def test_invalid_python_override_blocks(self) -> None:
        self.environment["MYPY_PYTHON_EXECUTABLE"] = "missing-python"
        result = self.run_hook()
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("MYPY_PYTHON_EXECUTABLE is not executable", result.stderr)

    def test_filename_arguments_cannot_narrow_the_scope(self) -> None:
        result = self.run_hook("scripts/check.py")
        self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("does not accept filenames", result.stderr)

    def test_untracked_owned_python_is_not_silently_validated(self) -> None:
        self.write("ai/untracked.py", "bad: int = 'value'  # BAD\n")
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("ai/untracked.py", json.dumps(self.calls_checked()))

    def test_all_entry_points_use_the_same_blocking_runner(self) -> None:
        makefile = MAKEFILE.read_text()
        workflow = WORKFLOW.read_text()
        self.assertIn("python3 scripts/git-hooks/pre-push-mypy.py", makefile)
        self.assertIn("python3 scripts/git-hooks/pre-push-mypy.py", workflow)
        for forbidden in (
            "mypy advisory",
            "skipping advisory check",
            '|| echo "mypy',
            "\n\t-mypy ",
        ):
            self.assertNotIn(forbidden, makefile + workflow)

    def test_mypy_config_has_no_error_suppressions(self) -> None:
        config = MYPY_CONFIG.read_text()
        self.assertIn('python_version = "3.14"', config)
        for forbidden in (
            "ignore_errors",
            "ignore_missing_imports",
            "disable_error_code",
            'follow_imports = "skip"',
        ):
            self.assertNotIn(forbidden, config)

    def test_pre_commit_runs_complete_scope_and_contract(self) -> None:
        scope = hook_config("mypy-local")
        self.assertIn("always_run: true", scope)
        self.assertIn("pass_filenames: false", scope)
        contract = hook_config("mypy-scope-contract")
        self.assertIn("entry: python3 scripts/git-hooks/test-pre-push-mypy.py", contract)
        self.assertIn("stages: [pre-commit, pre-push]", contract)


if __name__ == "__main__":
    unittest.main()
