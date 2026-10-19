#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the master-push render check (ADR-3295).

Every case builds a disposable repository whose stub ``Makefile`` renders
``CHANGELOG.md`` from ``changelog.d/`` and checks it, commits with fixed
committer times and runs ``scripts/ci/check_render_on_master.py`` with a fixed
``--now``. A current render passes; an owed render passes inside the bound with
a warning and fails past it; a last render that is not the render of its
fragments fails at any age.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CHECK = ROOT / "scripts" / "ci" / "check_render_on_master.py"
WORKFLOW = ROOT / ".github" / "workflows" / "lint-and-format.yml"
GIT = shutil.which("git") or "/usr/bin/git"
NOW = 1_800_000_000
HOUR = 3600
IDENT = {
    "GIT_AUTHOR_NAME": "Ada",
    "GIT_AUTHOR_EMAIL": "ada@example.org",
    "GIT_COMMITTER_NAME": "Ada",
    "GIT_COMMITTER_EMAIL": "ada@example.org",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}
MAKEFILE = (
    ".PHONY: docs-render docs-render-check\n"
    "docs-render:\n"
    "\t@cat changelog.d/*/*.md > CHANGELOG.md\n"
    "docs-render-check:\n"
    "\t@cat changelog.d/*/*.md | cmp -s - CHANGELOG.md"
    " || { echo 'CHANGELOG.md is stale'; exit 1; }\n"
)


class RenderOnMasterTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "master")
        self.commit({"Makefile": MAKEFILE, "changelog.d/fixed/a.md": "- fixed a\n"}, age_h=100)
        self.commit({"CHANGELOG.md": "- fixed a\n"}, age_h=99)

    def git(self, *args: str, date: str | None = None) -> str:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        env.update(IDENT)
        if date is not None:
            env.update(GIT_AUTHOR_DATE=date, GIT_COMMITTER_DATE=date)
        done = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [GIT, *args], cwd=self.repo, env=env, capture_output=True, text=True, check=True
        )
        return done.stdout

    def commit(self, files: dict[str, str], age_h: float) -> None:
        for rel, content in files.items():
            path = self.repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "change", date=f"@{int(NOW - age_h * HOUR)} +0000")

    def check(self) -> subprocess.CompletedProcess[str]:
        env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
        return subprocess.run(  # noqa: S603 -- fixed argv, no shell, check under test
            [sys.executable, str(CHECK), "--root", str(self.repo), "--now", str(NOW)],
            env=env,
            capture_output=True,
            text=True,
            check=False,
        )

    def assert_status(
        self, res: subprocess.CompletedProcess[str], status: int, needle: str
    ) -> None:
        out = res.stdout + res.stderr
        self.assertEqual(res.returncode, status, out)
        self.assertIn(needle, out)

    # --- positive -------------------------------------------------------

    def test_current_render_passes(self) -> None:
        self.assert_status(self.check(), 0, "the render at HEAD is current")

    def test_owed_render_inside_the_bound_warns(self) -> None:
        self.commit({"changelog.d/fixed/b.md": "- fixed b\n"}, age_h=1)
        self.commit({"changelog.d/fixed/c.md": "- fixed c\n"}, age_h=0.5)
        self.assert_status(self.check(), 0, "::warning title=ADR-3295 rendered docs::render owed")

    def test_render_after_the_window_is_current_again(self) -> None:
        self.commit({"changelog.d/fixed/b.md": "- fixed b\n"}, age_h=30)
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n"}, age_h=29)
        self.assert_status(self.check(), 0, "the render at HEAD is current")

    def test_no_worktree_is_left_behind(self) -> None:
        self.commit({"changelog.d/fixed/b.md": "- fixed b\n"}, age_h=1)
        self.check()
        self.assertEqual(self.git("worktree", "list", "--porcelain").count("worktree "), 1)

    # --- planted negatives ----------------------------------------------

    def test_owed_render_past_the_bound_fails(self) -> None:
        # The owed time starts at the first landing after the render, not the last.
        self.commit({"changelog.d/fixed/b.md": "- fixed b\n"}, age_h=25)
        self.commit({"changelog.d/fixed/c.md": "- fixed c\n"}, age_h=1)
        self.assert_status(self.check(), 1, "::error title=ADR-3295 rendered docs::render owed")

    def test_hand_edited_last_render_fails_inside_the_bound(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- an edit no fragment carries\n"}, age_h=2)
        self.commit({"changelog.d/fixed/b.md": "- fixed b\n"}, age_h=1)
        self.assert_status(self.check(), 1, "is not the render of its fragments")

    def test_hand_edit_at_head_fails(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- an edit no fragment carries\n"}, age_h=1)
        self.assert_status(self.check(), 1, "is not the render of its fragments")

    def test_no_render_commit_fails(self) -> None:
        shutil.rmtree(self.repo / ".git")
        (self.repo / "CHANGELOG.md").unlink()
        self.git("init", "-q", "-b", "master")
        self.commit({"Makefile": MAKEFILE, "changelog.d/fixed/a.md": "- fixed a\n"}, age_h=1)
        self.assert_status(self.check(), 1, "no commit of HEAD carries a render")


class RenderOnMasterWiringTest(unittest.TestCase):
    def test_master_push_step_runs_the_check(self) -> None:
        text = WORKFLOW.read_text()
        self.assertIn("python3 scripts/ci/check_render_on_master.py", text)
        self.assertNotIn("run: make docs-render-check\n", text)


if __name__ == "__main__":
    unittest.main()
