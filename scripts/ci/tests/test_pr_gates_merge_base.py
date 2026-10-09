#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The pull request gates read the diff from the merge base, not from BASE_SHA.

GitHub's ``pull_request.base.sha`` is the base branch tip when the event fired.
A pull request that fell behind it was judged on every file master changed
since the fork: the deliverables gate refused it for the rendered files
(ADR-2197) master re-renders on every landing, and the ``docs/state.md`` gate
passed it for a state row master added. Each case builds a repository whose
master moved after the pull request branched, runs the gate with
``BASE_SHA`` = master's tip, and asserts the verdict the pull request's own
change deserves. A base with no merge base fails closed.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
CI = ROOT / "scripts" / "ci"
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
FULL = (
    "- [ ] **Research digest** — no digest needed: trivial\n"
    "- [ ] **Decision matrix** — no alternatives: only-one-way fix\n"
    "- [ ] **AGENTS.md invariant note** — no rebase-sensitive invariants\n"
    "- [x] **Reproducer / smoke-test command** — below\n"
    "- [x] **CHANGELOG fragment** — changelog.d/fixed/x.md\n"
    "- [ ] **Rebase note** — no rebase impact: tooling only\n"
)
SMALL = (
    "small PR (ADR-2461)\n"
    "- [x] **Reproducer / smoke-test command** — below\n"
    "- [x] **CHANGELOG fragment** — changelog.d/fixed/x.md\n"
)
HEADER = "core/include/libvmaf/libvmaf.h"
PATCH = "ffmpeg-patches/0001-libvmaf.patch"


class BehindBase(unittest.TestCase):
    """A pull request branched from ``fork``; master then moved to ``tip``."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "master")
        self.commit(
            "base",
            {
                "README.md": "base\n",
                "CHANGELOG.md": "# Changelog\n",
                "docs/adr/README.md": "# ADRs\n",
                "docs/state.md": "# state\n",
                HEADER: "int vmaf_widget_open(void);\n",
                PATCH: "+  ret = vmaf_widget_open();\n",
            },
        )
        self.fork = self.rev("HEAD")

    def git(self, *args: str) -> str:
        env = {**os.environ, **IDENT}
        done = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [GIT, *args], cwd=self.repo, env=env, capture_output=True, text=True, check=True
        )
        return done.stdout

    def rev(self, ref: str) -> str:
        return self.git("rev-parse", ref).strip()

    def commit(self, message: str, files: dict[str, str]) -> None:
        for rel, content in files.items():
            path = self.repo / rel
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content)
        self.git("add", "-A")
        self.git("commit", "-q", "-m", message)

    def pull_request(self, files: dict[str, str]) -> str:
        """Branch from the fork point, commit ``files``; return the head."""
        self.git("checkout", "-q", "-b", "pr", self.fork)
        self.commit("change", {"changelog.d/fixed/x.md": "- fixed a thing\n", **files})
        head = self.rev("HEAD")
        self.git("checkout", "-q", "master")
        return head

    def master_moves(self, files: dict[str, str]) -> str:
        self.git("checkout", "-q", "master")
        self.commit("landed elsewhere", files)
        return self.rev("HEAD")

    def run_gate(
        self, script: str, base: str, head: str, **env: str
    ) -> subprocess.CompletedProcess[str]:
        clean = {k: v for k, v in os.environ.items() if k not in ("PR_BODY", "PR_TITLE")}
        clean.update(BASE_SHA=base, HEAD_SHA=head, **env)
        return subprocess.run(  # noqa: S603 -- fixed argv, no shell, gate under test
            [BASH, str(CI / script)],
            cwd=self.repo,
            env=clean,
            capture_output=True,
            text=True,
            check=False,
            stdin=subprocess.DEVNULL,
        )

    # --- deliverables-check.sh ---------------------------------------------

    def test_rendered_files_master_changed_are_not_the_pull_requests(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        tip = self.master_moves(
            {"CHANGELOG.md": "# Changelog\n\n## 1.0\n", "docs/adr/README.md": "x\n"}
        )
        res = self.run_gate("deliverables-check.sh", tip, head, PR_BODY=FULL)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertNotIn("ADR-2197 rendered file", res.stdout)

    def test_a_pull_request_that_edits_a_rendered_file_still_fails(self) -> None:
        head = self.pull_request({"CHANGELOG.md": "# Changelog\n\nhand edit\n"})
        tip = self.master_moves({"README.md": "moved\n"})
        res = self.run_gate("deliverables-check.sh", tip, head, PR_BODY=FULL)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("ADR-2197 rendered file", res.stdout)
        self.assertIn("CHANGELOG.md", res.stdout)

    def test_the_small_pr_counter_counts_only_the_pull_requests_lines(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        big = "".join(f"line {i}\n" for i in range(300))
        tip = self.master_moves({"core/src/landed.c": big, "python/test/landed.py": big})
        res = self.run_gate("deliverables-check.sh", tip, head, PR_BODY=SMALL)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("small PR track", res.stdout)

    def test_no_merge_base_fails_closed(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        self.git("checkout", "-q", "--orphan", "elsewhere")
        self.commit("unrelated", {"README.md": "unrelated\n"})
        unrelated = self.rev("HEAD")
        self.git("checkout", "-q", "master")
        for script in (
            "deliverables-check.sh",
            "state-md-touch-check.sh",
            "ffmpeg-patches-surface-check.sh",
        ):
            with self.subTest(script=script):
                res = self.run_gate(script, unrelated, head, PR_BODY=FULL, PR_TITLE="fix: x")
                self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                self.assertIn("no merge base", res.stderr)

    # --- state-md-touch-check.sh -------------------------------------------

    def test_a_state_row_master_added_does_not_count_for_the_pull_request(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        tip = self.master_moves(
            {"docs/state.md": "# state\n\n| **T-X-2026-10-10** | closed by PR #1 | x |\n"}
        )
        res = self.run_gate(
            "state-md-touch-check.sh", tip, head, PR_TITLE="fix: a bug", PR_BODY="Body."
        )
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)

    def test_the_pull_requests_own_state_row_still_counts(self) -> None:
        head = self.pull_request(
            {"docs/state.md": "# state\n\n| **T-Y-2026-10-10** | closed by PR #2 | y |\n"}
        )
        tip = self.master_moves({"README.md": "moved\n"})
        res = self.run_gate(
            "state-md-touch-check.sh", tip, head, PR_TITLE="fix: a bug", PR_BODY="Body."
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    # --- ffmpeg-patches-surface-check.sh -----------------------------------

    def test_a_header_change_master_landed_needs_no_patch_update(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        tip = self.master_moves({HEADER: "int vmaf_widget_open(int flags);\n"})
        res = self.run_gate("ffmpeg-patches-surface-check.sh", tip, head, PR_BODY="Body.")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_the_pull_requests_own_header_change_still_needs_one(self) -> None:
        head = self.pull_request({HEADER: "int vmaf_widget_open(int flags);\n"})
        tip = self.master_moves({"README.md": "moved\n"})
        res = self.run_gate("ffmpeg-patches-surface-check.sh", tip, head, PR_BODY="Body.")
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)

    # --- classify-dependency-pr.sh and release-pr-exempt.sh ----------------

    def test_the_bot_classifier_has_no_silent_two_dot_fallback(self) -> None:
        head = self.pull_request({"scripts/tool.py": "print(1)\n"})
        self.git("checkout", "-q", "--orphan", "elsewhere")
        self.commit("unrelated", {"README.md": "unrelated\n"})
        unrelated = self.rev("HEAD")
        self.git("checkout", "-q", "master")
        res = self.run_gate(
            "classify-dependency-pr.sh",
            unrelated,
            head,
            PR_AUTHOR="renovate[bot]",
            HEAD_REF="renovate/x",
        )
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("no merge base", res.stderr)
        res = self.run_gate(
            "release-pr-exempt.sh",
            unrelated,
            head,
            HEAD_REF="release-please--branches--master--components--vmafx",
            PR_AUTHOR="lusoris",
            PR_AUTHOR_TYPE="User",
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("exempt=false", res.stdout)
        self.assertIn("no merge base", res.stderr)


if __name__ == "__main__":
    unittest.main()
