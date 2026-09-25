#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise merge-base ownership and fail-closed pre-push selection with real Git."""

from __future__ import annotations

import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import TextCommandResult
from scripts.lib.safe_subprocess import run as run_command

SCRIPT = Path(__file__).with_name("pre-push-mypy.py").resolve()
HELPER = SCRIPT.parents[1] / "lib/safe_subprocess.py"
CONFIG = SCRIPT.parents[2] / ".pre-commit-config.yaml"
ROOT = SCRIPT.parents[2]
GIT = shutil.which("git") or "/usr/bin/git"
PRE_COMMIT = shutil.which("pre-commit") or "/usr/bin/pre-commit"
MYPY = (
    str(Path(sys.executable).with_name("mypy"))
    if Path(sys.executable).with_name("mypy").is_file()
    else shutil.which("mypy")
)
MYPY_ISOLATION_ARGS = ["--no-site-packages", "--disable-error-code=import-not-found"]


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
        # Present at the merge base and never touched: the inherited finding.
        self.write("scripts/debt.py", 'unchanged: int = "debt"  # BAD\n')
        self.commit("base")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        self.git("switch", "-qc", "feature")
        binary = self.directory / "bin"
        binary.mkdir()
        self.checker = binary / "mypy"
        self.receipt = self.directory / "checked.json"
        self.calls = self.directory / "calls.jsonl"
        # A stand-in for mypy: it records its argument vector, and reports a
        # finding for every file whose content carries the BAD marker. Findings
        # therefore follow file content, so the same stand-in produces the
        # branch's findings at HEAD and the inherited ones at the merge base.
        self.checker.write_text(
            f"#!{sys.executable}\n"
            "import json, os, pathlib, sys\n"
            "args = [a for a in sys.argv[1:] if not a.startswith('--')]\n"
            'pathlib.Path(os.environ["CHECKED_PATH"]).write_text(json.dumps(args))\n'
            "with open(os.environ['CHECKED_CALLS'], 'a') as handle:\n"
            "    handle.write(json.dumps(sys.argv[1:]) + chr(10))\n"
            "found = 0\n"
            "for name in args:\n"
            "    text = pathlib.Path(name).read_text()\n"
            "    for number, line in enumerate(text.splitlines(), start=1):\n"
            "        if 'BAD' in line:\n"
            "            found += 1\n"
            "            message = line.split('BAD', 1)[1].strip(': ') or 'planted finding'\n"
            "            print(f'{name}:{number}: error: {message}  [assignment]')\n"
            'print("CHECKED", *args)\n'
            "status = int(os.environ.get('CHECKER_STATUS', '0'))\n"
            "raise SystemExit(status or (1 if found else 0))\n"
        )
        self.checker.chmod(0o700)
        self.environment["PATH"] = str(binary) + os.pathsep + self.environment["PATH"]
        self.environment["CHECKED_PATH"] = str(self.receipt)
        self.environment["CHECKED_CALLS"] = str(self.calls)
        # Keep the baseline worktree the hook creates inside the fixture.
        self.environment["XDG_CACHE_HOME"] = str(self.directory / "cache")

    def git(self, *args: str) -> str:
        result = run_command(
            [GIT, "-C", str(self.root), *args],
            allowed_executables=(GIT,),
            env=self.environment,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=60,
        )
        assert isinstance(result.stdout, str)
        return result.stdout.strip()

    def write(self, filename: str, content: str = "owned: int = 1\n") -> Path:
        path = self.root / filename
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def commit(self, message: str) -> None:
        self.git("add", "-A")
        self.git("commit", "-qm", message)

    def run_hook(self, *args: str) -> TextCommandResult:
        return run_command(
            [sys.executable, str(SCRIPT), *args],
            allowed_executables=(sys.executable,),
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=300,
        )

    def run_framework(self, from_ref: str, to_ref: str) -> TextCommandResult:
        """Drive the shipped hook configuration through the installed framework."""
        return run_command(
            [
                PRE_COMMIT,
                "run",
                "mypy-local",
                "--hook-stage",
                "pre-push",
                "--from-ref",
                from_ref,
                "--to-ref",
                to_ref,
            ],
            allowed_executables=(PRE_COMMIT,),
            cwd=self.root,
            env=self.environment,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=300,
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
        self.write("scripts/owned.py", "from api import value\nowned: int = value()  # BAD\n")
        # Use the shipped hook configuration so missing always_run or restored
        # filename intersection is caught by the framework, not just unit calls.
        hook = hook_config("mypy-local")
        self.write("scripts/git-hooks/pre-push-mypy.py", SCRIPT.read_text())
        self.write("scripts/lib/safe_subprocess.py", HELPER.read_text())
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
        result = self.run_framework(old_tip, "HEAD")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertEqual(
            json.loads(self.receipt.read_text()),
            [
                "scripts/git-hooks/pre-push-mypy.py",
                "scripts/lib/safe_subprocess.py",
                "scripts/owned.py",
            ],
        )
        # An empty old-tip/new-tip file list must still recheck the owned set.
        result = self.run_framework("HEAD", "HEAD")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("scripts/owned.py", result.stdout)

    def test_type_change_from_symlink_is_checked(self) -> None:
        link = self.root / "scripts/owned.py"
        link.symlink_to("api.py")
        self.commit("base symlink")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        link.unlink()
        self.write("scripts/owned.py", 'owned: int = "invalid"  # BAD\n')
        self.commit("replace link with source")
        self.assertEqual(
            self.git("diff", "--name-status", "origin/master", "HEAD"), "T\tscripts/owned.py"
        )
        self.assert_checked(["scripts/owned.py"], 1)

    def test_symlink_keeps_lexical_identity(self) -> None:
        (self.root / "scripts/owned.py").symlink_to("debt.py")
        self.commit("new link to unchanged debt")
        # The finding is inherited content, but `scripts/owned.py` is a new path:
        # it has nothing at the merge base, so the finding counts as introduced.
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

    def test_checker_failing_without_a_finding_fails_closed(self) -> None:
        """An exit code we cannot attribute to a file is mypy breaking, not a pass."""
        self.write("scripts/owned.py")
        self.commit("owned source")
        self.environment["PRE_COMMIT_TO_REF"] = self.git("rev-parse", "HEAD")
        self.environment["CHECKER_STATUS"] = "7"
        result = self.run_hook()
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("without reporting a finding", result.stderr)

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

    def test_inherited_finding_is_not_the_branch_bug(self) -> None:
        """Editing a file that already had a finding must not fail the push."""
        self.write("scripts/debt.py", 'unchanged: int = "debt"  # BAD\nadded: int = 1\n')
        self.commit("append a clean line to a file that already had a finding")
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 inherited", result.stdout)

    def test_finding_that_only_moved_lines_is_still_inherited(self) -> None:
        """A line number is not part of a finding's identity."""
        self.write("scripts/debt.py", 'added: int = 1\nunchanged: int = "debt"  # BAD\n')
        self.commit("insert a line above the existing finding")
        result = self.run_hook()
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("1 inherited", result.stdout)

    def test_introduced_finding_fails_and_is_named(self) -> None:
        self.write("scripts/owned.py", "planted: int = 1  # BAD\n")
        self.commit("add a file with a finding")
        result = self.run_hook()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("scripts/owned.py", result.stderr)
        self.assertIn("[assignment]", result.stderr)
        self.assertNotIn("scripts/debt.py", result.stderr)

    def test_second_finding_in_an_already_failing_file_is_reported(self) -> None:
        """Inheritance is per finding, not per file: a file may already fail."""
        self.write(
            "scripts/debt.py",
            'unchanged: int = "debt"  # BAD\nother: int = 2  # BAD: a different problem\n',
        )
        self.commit("add a second, different finding to a file that already had one")
        result = self.run_hook()
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("a different problem", result.stderr)
        self.assertNotIn("planted finding", result.stderr)

    def test_package_base_files_are_checked_with_explicit_bases(self) -> None:
        """ai/src is a mypy_path base: without the flag mypy refuses the file."""
        self.write("ai/src/aiutils/mod.py", "value: int = 1\n")
        self.write("scripts/owned.py", "value: int = 1\n")
        self.commit("touch one file under the package base and one outside it")
        self.assertEqual(self.run_hook().returncode, 0)
        calls = [json.loads(line) for line in self.calls.read_text().splitlines()]
        based = [call for call in calls if "--explicit-package-bases" in call]
        plain = [call for call in calls if "--explicit-package-bases" not in call]
        self.assertEqual(
            based,
            [[*MYPY_ISOLATION_ARGS, "--explicit-package-bases", "ai/src/aiutils/mod.py"]],
        )
        self.assertEqual(plain, [[*MYPY_ISOLATION_ARGS, "scripts/owned.py"]])

    def test_baseline_worktree_is_always_removed(self) -> None:
        self.write("scripts/owned.py", "planted: int = 1  # BAD\n")
        self.commit("add a file with a finding")
        self.assertEqual(self.run_hook().returncode, 1)
        self.assertNotIn("vmafx-mypy-baseline", self.git("worktree", "list"))
        cache = self.directory / "cache"
        leftovers = [p.name for p in cache.iterdir()] if cache.exists() else []
        self.assertEqual(leftovers, [], f"baseline worktree left behind: {leftovers}")

    def test_regression_hook_is_registered_for_local_and_ci_checks(self) -> None:
        hook = hook_config("mypy-scope-contract")
        self.assertIn("entry: python3 scripts/git-hooks/test-pre-push-mypy.py", hook)
        self.assertIn("stages: [pre-commit, pre-push]", hook)
        self.assertIn("pass_filenames: false", hook)
        scope = hook_config("mypy-local")
        self.assertIn("always_run: true", scope)
        self.assertIn("pass_filenames: false", scope)
        self.assertNotIn("\n        files:", scope)


@unittest.skipUnless(MYPY, "mypy is not installed")
class MypyModuleIdentity(unittest.TestCase):
    def test_helper_and_hook_have_one_module_identity(self) -> None:
        """Simultaneous roots must not name safe_subprocess twice."""
        assert MYPY is not None
        result = run_command(
            [
                MYPY,
                *MYPY_ISOLATION_ARGS,
                str(HELPER.relative_to(ROOT)),
                str(SCRIPT.relative_to(ROOT)),
            ],
            allowed_executables=(MYPY,),
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=60,
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_sidecar_tests_have_no_untyped_decorators(self) -> None:
        """Pytest decorators under --no-site-packages must be typed aliases (ADR-1261)."""
        assert MYPY is not None
        result = run_command(
            [
                MYPY,
                *MYPY_ISOLATION_ARGS,
                "ai/sidecar/tests/test_online_trainer.py",
                "ai/sidecar/tests/test_socket_permissions.py",
            ],
            allowed_executables=(MYPY,),
            cwd=ROOT,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=60,
        )
        # Inherited baseline debt exists, but no untyped-decorator errors may be present.
        self.assertNotIn(
            "untyped-decorator",
            result.stdout + result.stderr,
            f"Found untyped decorator findings under hermetic mypy:\n{result.stdout}\n{result.stderr}",
        )


if __name__ == "__main__":
    unittest.main()
