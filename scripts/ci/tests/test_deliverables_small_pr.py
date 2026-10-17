#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the small-PR track of the deliverables gate (ADR-2461).

Every case builds a disposable repository with a base commit and a branch, runs
``scripts/ci/deliverables-check.sh`` over it and asserts the exit status.  The
marker ``small PR (ADR-2461)`` waives the research digest, decision matrix,
``AGENTS.md`` note and rebase note for a pull request that qualifies; a pull
request that does not qualify is refused *even with the marker*, and a pull
request without the marker is judged exactly as before.
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

MARKER = "small PR (ADR-2461)"
KEPT = (
    "- [x] **Reproducer / smoke-test command** — below\n"
    "- [x] **CHANGELOG fragment** — changelog.d/fixed/x.md\n"
)
FULL = (
    "- [ ] **Research digest** — no digest needed: trivial\n"
    "- [ ] **Decision matrix** — no alternatives: only-one-way fix\n"
    "- [ ] **AGENTS.md invariant note** — no rebase-sensitive invariants\n"
    "- [x] **Reproducer / smoke-test command** — below\n"
    "- [x] **CHANGELOG fragment** — changelog.d/fixed/x.md\n"
    "- [ ] **Rebase note** — no rebase impact: docs-only\n"
)


def lines(n: int) -> str:
    return "".join(f"line {i}\n" for i in range(n))


class SmallPrGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Path(self._tmp.name)
        self.git("init", "-q", "-b", "main")
        self.write("README.md", "base\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "base")
        self.base = self.git("rev-parse", "HEAD").strip()

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

    def branch(self, files: dict[str, str]) -> None:
        for rel, content in files.items():
            self.write(rel, content)
        self.write("changelog.d/fixed/x.md", "- fixed a thing\n")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "change")

    def gate(self, body: str) -> subprocess.CompletedProcess[str]:
        env = {k: v for k, v in os.environ.items() if k not in ("PR_BODY", "BASE_SHA", "HEAD_SHA")}
        env.update(PR_BODY=body, BASE_SHA=self.base, HEAD_SHA=self.git("rev-parse", "HEAD").strip())
        return subprocess.run(  # noqa: S603 -- fixed argv, no shell, gate under test
            [BASH, str(GATE)], cwd=self.repo, env=env, capture_output=True, text=True, check=False
        )

    def small(self, extra: str = "") -> str:
        return f"{MARKER}\n{KEPT}{extra}"

    def assert_refused(self, res: subprocess.CompletedProcess[str], needle: str) -> None:
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("ADR-2461", res.stdout + res.stderr)
        self.assertIn(needle, res.stdout + res.stderr)

    # --- positive -------------------------------------------------------

    def test_qualifying_pr_with_marker_passes_without_the_four_waived_items(self) -> None:
        self.branch({"docs/usage/cli.md": lines(5)})
        res = self.gate(self.small())
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("small PR track", res.stdout)

    def test_marker_may_sit_in_backticks(self) -> None:
        self.branch({"docs/usage/cli.md": lines(5)})
        self.assertEqual(self.gate(f"`{MARKER}`\n{KEPT}").returncode, 0)

    def test_code_fix_in_one_subtree_with_a_test_qualifies(self) -> None:
        self.branch({"scripts/ci/foo.py": lines(20), "scripts/ci/tests/test_foo.py": lines(20)})
        self.assertEqual(self.gate(self.small()).returncode, 0)

    # --- unchanged behaviour without the marker -------------------------

    def test_without_the_marker_the_four_items_are_still_required(self) -> None:
        self.branch({"docs/usage/cli.md": lines(5)})
        res = self.gate(KEPT)
        self.assertEqual(res.returncode, 1, res.stdout)
        self.assertIn("Research digest is neither ticked nor opted-out", res.stdout)

    def test_full_checklist_without_the_marker_still_passes_on_a_large_change(self) -> None:
        self.branch({"core/src/big.c": lines(500)})
        self.assertEqual(self.gate(FULL).returncode, 0)

    # --- kept deliverables ----------------------------------------------

    def test_marker_does_not_waive_the_reproducer(self) -> None:
        self.branch({"docs/usage/cli.md": lines(5)})
        body = f"{MARKER}\n- [x] **CHANGELOG fragment** — changelog.d/fixed/x.md\n"
        res = self.gate(body)
        self.assertEqual(res.returncode, 1, res.stdout)
        self.assertIn("Reproducer", res.stdout)

    def test_marker_does_not_waive_the_changelog_fragment(self) -> None:
        self.branch({"docs/usage/cli.md": lines(5)})
        body = f"{MARKER}\n- [x] **Reproducer / smoke-test command** — below\n"
        res = self.gate(body)
        self.assertEqual(res.returncode, 1, res.stdout)
        self.assertIn("CHANGELOG fragment", res.stdout)

    # --- non-qualifying: refused even with the marker -------------------

    def test_core_src_is_refused_with_the_marker(self) -> None:
        self.branch({"core/src/feature/x.c": lines(5)})
        self.assert_refused(self.gate(self.small()), "core/src/feature/x.c")

    def test_public_header_is_refused_with_the_marker(self) -> None:
        self.branch({"core/include/libvmaf/x.h": lines(5)})
        self.assert_refused(self.gate(self.small()), "core/include/libvmaf/x.h")

    def test_golden_tests_and_ffmpeg_patches_and_cli_and_options_are_refused(self) -> None:
        for rel in (
            "python/test/quality_runner_test.py",
            "ffmpeg-patches/0001-x.patch",
            "core/tools/cli_parse.cpp",
            "core/meson_options.txt",
        ):
            with self.subTest(rel=rel):
                self.git("checkout", "-q", "-B", "work", self.base)
                self.branch({rel: lines(3)})
                self.assert_refused(self.gate(self.small()), rel)

    def test_a_new_adr_is_refused_with_the_marker(self) -> None:
        self.branch({"docs/adr/9999-new.md": lines(5)})
        self.assert_refused(self.gate(self.small()), "docs/adr/9999-new.md")

    def test_two_top_level_directories_are_refused(self) -> None:
        self.branch({"scripts/ci/foo.py": lines(5), "tools/bar.py": lines(5)})
        self.assert_refused(self.gate(self.small()), "one top-level")

    def test_more_than_100_changed_lines_are_refused_and_100_is_not(self) -> None:
        self.branch({"scripts/ci/foo.py": lines(101)})
        self.assert_refused(self.gate(self.small()), "101")
        self.git("checkout", "-q", "-B", "work", self.base)
        self.branch({"scripts/ci/foo.py": lines(100)})
        self.assertEqual(self.gate(self.small()).returncode, 0)

    def test_fragments_and_lock_files_do_not_count_toward_the_size(self) -> None:
        self.branch(
            {
                "scripts/ci/foo.py": lines(100),
                "changelog.d/fixed/y.md": lines(300),
                "docs/rebase-notes.d/z.md": lines(300),
                "requirements/locks/tooling.txt": lines(300),
                "go.sum": lines(300),
            }
        )
        self.assertEqual(self.gate(self.small()).returncode, 0)

    def test_docs_beside_one_code_directory_count_as_one_subtree(self) -> None:
        self.branch({"scripts/ci/foo.py": lines(10), "docs/development/foo.md": lines(10)})
        self.assertEqual(self.gate(self.small()).returncode, 0)


class SmallPrWiringTest(unittest.TestCase):
    def test_template_and_guide_name_the_marker(self) -> None:
        for rel in (
            ".github/PULL_REQUEST_TEMPLATE.md",
            "docs/development/pr-body-sentinel-guide.md",
        ):
            self.assertIn(MARKER, (ROOT / rel).read_text(encoding="utf-8"), rel)


if __name__ == "__main__":
    unittest.main()
