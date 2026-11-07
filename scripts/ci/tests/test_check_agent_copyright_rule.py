#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Positive, planted negative and boundary cases for the ADR-0861 rule check."""

from __future__ import annotations

import io
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from pathlib import Path
from unittest import mock

import check_agent_copyright_rule as rule

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))
from scripts.lib import git_env


def fixture_git_env() -> dict[str, str]:
    """Return a hermetic environment for disposable fixture repositories."""
    environment = git_env.drop()
    environment.update(GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
    return environment


class AgentCopyrightRuleTest(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        # The caller's environment, kept for the real-tree check: a partial commit's hook
        # exports GIT_INDEX_FILE for the commit's own index, and the checker must read it.
        self.caller_env = dict(os.environ)
        # A commit hook exports GIT_INDEX_FILE (and may export GIT_DIR); fixture git and the
        # checker's own `git ls-files` must never reach the caller's repository.
        hermetic = mock.patch.dict(os.environ, fixture_git_env(), clear=True)
        hermetic.start()
        self.addCleanup(hermetic.stop)
        self.git("init")

    def git(self, *args: str) -> None:
        rule.run_command(
            ["git", *args],
            allowed_executables=("git",),
            cwd=self.root,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=30,
        )

    def plant(self, name: str, text: str) -> None:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        self.git("add", "--", name)

    def run_on(self, root: Path | None = None) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with redirect_stdout(out), redirect_stderr(err):
            status = rule.main(["--root", str(self.root if root is None else root)])
        return status, out.getvalue(), err.getvalue()

    def test_checked_in_tree_passes(self) -> None:
        with mock.patch.dict(os.environ, self.caller_env, clear=True):
            status, out, err = self.run_on(rule.ROOT)
        self.assertEqual(status, 0, err)
        self.assertIn("tracked files checked", out)

    def test_tracked_rules_passes_hook_index_to_real_tree_only(self) -> None:
        completed = mock.Mock(returncode=0, stdout="", stderr="")
        caller = {**self.caller_env, "GIT_INDEX_FILE": "partial-commit-index"}
        with (
            mock.patch.dict(os.environ, caller, clear=True),
            mock.patch.object(rule, "run_command", return_value=completed) as run,
        ):
            rule.tracked_rules(rule.ROOT)
            real_env = run.call_args.kwargs["env"]
            rule.tracked_rules(self.root)
            fixture_env = run.call_args.kwargs["env"]
        self.assertEqual(real_env["GIT_INDEX_FILE"], "partial-commit-index")
        self.assertNotIn("GIT_INDEX_FILE", fixture_env)

    def test_planted_dual_rule_fails_with_path_and_line(self) -> None:
        self.plant("AGENTS.md", "# Rules\nship dual Lusoris/Claude (Anthropic) copyright header\n")
        status, _, err = self.run_on()
        self.assertEqual(status, 1)
        self.assertIn("AGENTS.md:2: ship dual Lusoris/Claude (Anthropic)", err)

    def test_planted_plus_form_fails(self) -> None:
        self.plant("AGENTS.md", "Lusoris+Claude-only\n")
        self.assertEqual(self.run_on()[0], 1)

    def test_case_whitespace_and_dual_without_slash_fail(self) -> None:
        for text in ("LUSORIS / cLaUdE", "Lusoris  +  Claude", "DUAL\tLUSORIS notice"):
            with self.subTest(text=text):
                self.plant("AGENTS.md", text)
                self.assertEqual(self.run_on()[0], 1)

    def test_each_rule_surface_is_scanned(self) -> None:
        names = (
            "AGENTS.md",
            "nested/deep/AGENTS.md",
            "nested/deep/AGENTS.d/rule.md",
            "CONTRIBUTING.md",
            "docs/development/rule.md",
        )
        for name in names:
            self.plant(name, "Lusoris+Claude-only\n")
        status, _, err = self.run_on()
        self.assertEqual(status, 1)
        for name in names:
            self.assertIn(f"{name}:1:", err)

    def test_legacy_acceptance_wording_passes(self) -> None:
        self.plant("AGENTS.md", 'accept legacy "Lusoris and Claude (Anthropic)" form\n')
        status, out, err = self.run_on()
        self.assertEqual(status, 0, err)
        self.assertIn("1 tracked files checked", out)

    def test_outside_scan_set_passes(self) -> None:
        self.plant("docs/adr/history.md", "Lusoris+Claude-only\n")
        self.assertEqual(self.run_on()[0], 0)

    def test_untracked_rule_is_not_scanned(self) -> None:
        (self.root / "AGENTS.md").write_text("Lusoris+Claude-only\n", encoding="utf-8")
        self.assertEqual(self.run_on()[0], 0)

    def test_unreadable_root_exits_2(self) -> None:
        status, _, err = self.run_on(self.root / "missing")
        self.assertEqual(status, 2)
        self.assertIn("missing", err)

    def test_non_repository_exits_2_with_git_cause(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            status, _, err = self.run_on(Path(directory))
        self.assertEqual(status, 2)
        self.assertIn("git ls-files exited", err)

    def test_unreadable_tracked_file_exits_2(self) -> None:
        self.plant("AGENTS.md", "Copyright 2026 Lusoris\n")
        with mock.patch.object(Path, "read_text", side_effect=PermissionError("AGENTS.md denied")):
            status, _, err = self.run_on()
        self.assertEqual(status, 2)
        self.assertIn("AGENTS.md denied", err)

    def test_caller_repository_variables_are_dropped(self) -> None:
        with tempfile.TemporaryDirectory() as caller:
            index = Path(caller) / "caller-index"
            git_dir = Path(caller) / "caller.git"
            inherited = {"GIT_INDEX_FILE": str(index), "GIT_DIR": str(git_dir)}
            with mock.patch.dict(os.environ, inherited):
                hermetic = mock.patch.dict(os.environ, fixture_git_env(), clear=True)
                with hermetic:
                    self.plant("AGENTS.md", "Lusoris+Claude-only\n")
                    status = self.run_on()[0]
            self.assertEqual(status, 1)
            self.assertFalse(index.exists())
            self.assertFalse(git_dir.exists())

    def caller_repository(self, caller: Path) -> dict[str, str]:
        """Build a clean tracked-AGENTS.md repository; return the hook variables naming it."""
        caller.mkdir()
        for args in (("init",), ("add", "--", "AGENTS.md")):
            if args[0] == "add":
                (caller / "AGENTS.md").write_text("Copyright 2026 Lusoris\n", encoding="utf-8")
            rule.run_command(
                ["git", *args],
                allowed_executables=("git",),
                cwd=caller,
                env=fixture_git_env(),
                capture_output=True,
                text=True,
                check=True,
                timeout_seconds=30,
            )
        return {
            "GIT_DIR": str(caller / ".git"),
            "GIT_INDEX_FILE": str(caller / ".git" / "index"),
        }

    def test_caller_hook_variables_do_not_redirect_other_root(self) -> None:
        # Fixture B (self.root) tracks a clean AGENTS.md and a retired form in a second rule
        # file; caller A tracks only its own clean AGENTS.md and is named by the hook variables.
        # Without env scrubbing git lists A's index, B's second file is never scanned, rc 0.
        self.plant("AGENTS.md", "Copyright 2026 Lusoris\n")
        self.plant("docs/development/rule.md", "Lusoris+Claude-only\n")
        with tempfile.TemporaryDirectory() as directory:
            inherited = self.caller_repository(Path(directory) / "caller")
            with mock.patch.dict(os.environ, inherited):
                status, _, err = self.run_on()
        self.assertEqual(status, 1)
        self.assertIn("docs/development/rule.md:1: Lusoris+Claude-only", err)

    def test_caller_hook_variables_clean_other_root_passes(self) -> None:
        self.plant("AGENTS.md", "Copyright 2026 Lusoris\n")
        with tempfile.TemporaryDirectory() as directory:
            inherited = self.caller_repository(Path(directory) / "caller")
            with mock.patch.dict(os.environ, inherited):
                status, out, err = self.run_on()
        self.assertEqual(status, 0, err)
        self.assertIn("1 tracked files checked", out)

    def test_git_environment_keeps_variables_for_own_checkout_only(self) -> None:
        with mock.patch.dict(os.environ, {"GIT_INDEX_FILE": "staged"}):
            self.assertEqual(rule.git_environment(rule.ROOT)["GIT_INDEX_FILE"], "staged")
            self.assertNotIn("GIT_INDEX_FILE", rule.git_environment(self.root))

    def test_invalid_utf8_exits_2(self) -> None:
        self.plant("AGENTS.md", "Copyright 2026 Lusoris\n")
        (self.root / "AGENTS.md").write_bytes(b"\xff")
        self.assertEqual(self.run_on()[0], 2)


if __name__ == "__main__":
    unittest.main()
