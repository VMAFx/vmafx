#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the render-only pull request of the deliverables gate (ADR-3295).

A pull request whose diff touches only the files ``make docs-render`` writes
(ADR-2197) is accepted when its head passes ``make docs-render-check``. Every
case builds a disposable repository whose stub ``Makefile`` renders
``CHANGELOG.md`` from ``changelog.d/`` and checks it, runs
``scripts/ci/deliverables-check.sh`` over a branch and asserts the exit
status. A rendered file next to any other file, a rendered file that is not
the render of the fragments, and ``_order.txt`` stay refused.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "scripts" / "ci" / "deliverables-check.sh"
GIT = shutil.which("git") or "/usr/bin/git"
BASH = shutil.which("bash") or "/bin/bash"
IDENT = {
    "GIT_AUTHOR_NAME": "Ada",
    "GIT_AUTHOR_EMAIL": "ada@example.org",
    "GIT_COMMITTER_NAME": "Ada",
    "GIT_COMMITTER_EMAIL": "ada@example.org",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
}

# The stub render: CHANGELOG.md is the fragments in name order.
MAKEFILE = (
    ".PHONY: docs-render docs-render-check\n"
    "docs-render:\n"
    "\t@cat changelog.d/*/*.md > CHANGELOG.md\n"
    "docs-render-check:\n"
    "\t@cat changelog.d/*/*.md | cmp -s - CHANGELOG.md"
    " || { echo 'CHANGELOG.md is stale'; exit 1; }\n"
)
BODY = (
    "Render of the landed fragments.\n\n"
    "no docs needed: rendered output only\n"
    "no state delta: rendered output only\n"
)


class RenderOnlyGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name) / "repo"
        self.repo.mkdir()
        self.git("init", "-q", "-b", "main")
        self.write("Makefile", MAKEFILE)
        self.write("changelog.d/fixed/a.md", "- fixed a\n")
        self.write("CHANGELOG.md", "- fixed a\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        # A pull request lands its fragment; the render is owed.
        self.write("changelog.d/fixed/b.md", "- fixed b\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "land b")
        self.base = self.git("rev-parse", "HEAD").strip()
        self.git("checkout", "-q", "-b", "render")

    def git(self, *args: str) -> str:
        env = {**{k: v for k, v in os.environ.items() if not k.startswith("GIT_")}, **IDENT}
        done = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [GIT, *args], cwd=self.repo, env=env, capture_output=True, text=True, check=True
        )
        return done.stdout

    def write(self, rel: str, content: str) -> None:
        path = self.repo / rel
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)

    def commit(self, files: dict[str, str]) -> None:
        for rel, content in files.items():
            self.write(rel, content)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "change")

    def gate(self, body: str = BODY) -> subprocess.CompletedProcess[str]:
        env = {k: v for k, v in os.environ.items() if k not in ("PR_BODY", "BASE_SHA", "HEAD_SHA")}
        env = {k: v for k, v in env.items() if not k.startswith("GIT_")}
        env.update(PR_BODY=body, BASE_SHA=self.base, HEAD_SHA=self.git("rev-parse", "HEAD").strip())
        return subprocess.run(  # noqa: S603 -- fixed argv, no shell, gate under test
            [BASH, str(GATE)], cwd=self.repo, env=env, capture_output=True, text=True, check=False
        )

    def assert_refused(self, res: subprocess.CompletedProcess[str], needle: str) -> None:
        out = res.stdout + res.stderr
        self.assertEqual(res.returncode, 1, out)
        self.assertIn(needle, out)

    # --- positive -------------------------------------------------------

    def test_clean_render_passes_without_the_deliverables(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n"})
        res = self.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("render-only pull request (ADR-3295)", res.stdout)

    def test_the_check_runs_on_the_head_not_on_the_working_tree(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n"})
        # The job's working tree may hold anything; the head commit is judged.
        self.write("CHANGELOG.md", "dirty\n")
        self.assertEqual(self.gate().returncode, 0)
        self.assertEqual((self.repo / "CHANGELOG.md").read_text(), "dirty\n")

    def test_no_worktree_is_left_behind(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n"})
        self.gate()
        self.assertEqual(self.git("worktree", "list", "--porcelain").count("worktree "), 1)

    # --- planted negatives ----------------------------------------------

    def test_rendered_file_next_to_another_file_is_refused(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n", "scripts/x.py": "x = 1\n"})
        self.assert_refused(self.gate(), "ADR-2197 rendered file")

    def test_rendered_file_next_to_a_fragment_is_refused(self) -> None:
        self.commit(
            {
                "CHANGELOG.md": "- fixed a\n- fixed b\n- fixed c\n",
                "changelog.d/fixed/c.md": "- fixed c\n",
            }
        )
        self.assert_refused(self.gate(), "ADR-2197 rendered file")

    def test_hand_edited_changelog_is_refused(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n- an edit no fragment carries\n"})
        self.assert_refused(self.gate(), "docs-render-check")

    def test_stale_render_is_refused(self) -> None:
        # A render that leaves out a landed fragment is not the render.
        self.commit({"CHANGELOG.md": "- fixed a\n\n"})
        self.assert_refused(self.gate(), "docs-render-check")

    def test_order_file_is_refused_even_alone(self) -> None:
        self.commit({"docs/adr/_index_fragments/_order.txt": "0001-x.md\n"})
        self.assert_refused(self.gate(), "_order.txt")

    def test_a_failing_make_is_refused(self) -> None:
        self.commit({"CHANGELOG.md": "- fixed a\n- fixed b\n", "Makefile": "x\n"})
        # Makefile is not a rendered file, so the mixed-diff refusal fires first.
        self.assert_refused(self.gate(), "ADR-2197 rendered file")


class RenderOnlyWiringTest(unittest.TestCase):
    def test_gate_runs_the_render_check_on_the_head(self) -> None:
        text = GATE.read_text()
        self.assertIn("docs-render-check", text)
        self.assertIn("worktree add --detach", text)
        self.assertIn("ADR-3295", text)

    def test_makefile_keeps_the_render_check_target(self) -> None:
        self.assertIn("\ndocs-render-check:", (ROOT / "Makefile").read_text())

    def test_guides_name_the_render_only_route(self) -> None:
        for rel in (
            "docs/development/release.md",
            "scripts/ci/AGENTS.d/pr-body-validator.md",
            ".github/PULL_REQUEST_TEMPLATE.md",
        ):
            with self.subTest(rel=rel):
                self.assertIn("ADR-3295", (ROOT / rel).read_text())


if __name__ == "__main__":
    unittest.main()
