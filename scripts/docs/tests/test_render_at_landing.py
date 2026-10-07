#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""The generated docs are rendered at landing; a pull request carries fragments only (ADR-2197).

Cases: the landing order read from history, the rebase-note render (newest first,
first-render bootstrap, idempotence, lint), the ADR index order (frozen manifest,
then landing order), and the property the decision exists for: a pull request
that adds only fragments does not conflict with a moved master under a plain
three-way merge (no union driver, which GitHub does not apply), while one that
edits the rendered file does.
"""

from __future__ import annotations

import shutil
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

from scripts.lib.safe_subprocess import CommandResult
from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[3]
GIT = shutil.which("git")
BASH = shutil.which("bash")
ENV = {
    "GIT_AUTHOR_NAME": "t",
    "GIT_AUTHOR_EMAIL": "t@example.invalid",
    "GIT_COMMITTER_NAME": "t",
    "GIT_COMMITTER_EMAIL": "t@example.invalid",
    "GIT_CONFIG_GLOBAL": "/dev/null",
    "GIT_CONFIG_SYSTEM": "/dev/null",
    "PATH": "/usr/bin:/bin:/usr/local/bin",
    "HOME": "/nonexistent",
}
SCRIPTS = (
    "fragment-order.py",
    "concat-rebase-notes.sh",
    "concat-adr-index.sh",
    "check-adr-index.py",
)
TITLE = "# Rebase notes\n\nolder entry stays\n"


def stdout_text(result: CommandResult) -> str:
    """The captured stdout as text (the runner types it ``str | bytes``)."""
    out = result.stdout
    return out.decode() if isinstance(out, bytes) else out


def git(root: Path, *args: str, check: bool = True) -> CommandResult:
    assert GIT is not None
    return run_command(
        (GIT, "-C", str(root), *args),
        allowed_executables=(GIT,),
        env=ENV,
        text=True,
        capture_output=True,
        check=check,
        timeout_seconds=60,
    )


def shell(root: Path, script: str, *args: str) -> CommandResult:
    assert BASH is not None
    return run_command(
        (BASH, str(root / "scripts/docs" / script), *args),
        allowed_executables=(BASH,),
        env=ENV,
        text=True,
        capture_output=True,
        check=False,
        timeout_seconds=60,
    )


def python(root: Path, script: str, *args: str) -> CommandResult:
    exe = sys.executable
    return run_command(
        (exe, str(root / "scripts/docs" / script), *args),
        allowed_executables=(exe,),
        env=ENV,
        text=True,
        capture_output=True,
        check=False,
        timeout_seconds=60,
    )


class Fixture(unittest.TestCase):
    def setUp(self) -> None:
        self.temporary = tempfile.TemporaryDirectory(prefix="vmafx-render-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        (self.root / "scripts/docs").mkdir(parents=True)
        for name in SCRIPTS:
            shutil.copyfile(ROOT / "scripts/docs" / name, self.root / "scripts/docs" / name)
        shutil.copytree(ROOT / "scripts/lib", self.root / "scripts/lib")
        (self.root / "scripts/__init__.py").write_text("")
        git(self.root, "init", "-q", "-b", "master")
        self.notes = self.root / "docs/rebase-notes.d"
        self.notes.mkdir(parents=True)
        (self.notes / "_README.md").write_text("# not rendered\n")
        (self.root / "docs/rebase-notes.md").write_text(TITLE)
        self.commit("start")

    def commit(self, message: str) -> None:
        git(self.root, "add", "-A")
        git(self.root, "commit", "-q", "-m", message)

    def land(self, name: str, text: str) -> None:
        (self.notes / name).write_text(text)
        self.commit(f"add {name}")


class LandingOrder(Fixture):
    def order(self) -> list[str]:
        result = python(self.root, "fragment-order.py", str(self.notes))
        self.assertEqual(result.returncode, 0, result.stderr)
        return stdout_text(result).split()

    def test_the_order_is_the_order_the_files_landed_not_their_names(self) -> None:
        self.land("z-first.md", "## z\n")
        self.land("a-second.md", "## a\n")
        self.assertEqual(self.order(), ["z-first.md", "a-second.md"])

    def test_an_uncommitted_file_follows_the_landed_ones_by_name(self) -> None:
        self.land("b.md", "## b\n")
        (self.notes / "d.md").write_text("## d\n")
        (self.notes / "c.md").write_text("## c\n")
        self.assertEqual(self.order(), ["b.md", "c.md", "d.md"])

    def test_underscore_files_are_not_listed(self) -> None:
        self.land("n.md", "## n\n")
        self.assertEqual(self.order(), ["n.md"])

    def test_a_shallow_clone_fails_loudly_instead_of_guessing(self) -> None:
        self.land("a.md", "## a\n")
        self.land("b.md", "## b\n")
        shallow = self.root.parent / (self.root.name + "-shallow")
        self.addCleanup(shutil.rmtree, shallow, True)
        assert GIT is not None
        run_command(
            (GIT, "clone", "-q", "--depth", "1", f"file://{self.root}", str(shallow)),
            allowed_executables=(GIT,),
            env=ENV,
            capture_output=True,
            check=True,
            timeout_seconds=60,
        )
        result = python(shallow, "fragment-order.py", str(shallow / "docs/rebase-notes.d"))
        self.assertEqual(result.returncode, 2)
        self.assertIn("shallow", result.stderr)
        self.assertEqual(result.stdout, "")


class RebaseNotes(Fixture):
    def block(self) -> str:
        text = (self.root / "docs/rebase-notes.md").read_text()
        return text.split("fragments:begin", 1)[1].split("fragments:end", 1)[0]

    def test_first_render_adds_the_block_and_lists_newest_first(self) -> None:
        self.land("old.md", "## Old (2026-01-01)\n\nold body\n")
        self.land("new.md", "## New (2026-02-01)\n\nnew body\n")
        self.assertEqual(shell(self.root, "concat-rebase-notes.sh", "--check").returncode, 1)
        self.assertEqual(shell(self.root, "concat-rebase-notes.sh", "--write").returncode, 0)
        block = self.block()
        self.assertLess(block.index("## New"), block.index("## Old"))
        text = (self.root / "docs/rebase-notes.md").read_text()
        self.assertTrue(text.endswith("older entry stays\n"))
        self.assertLess(text.index("fragments:end"), text.index("older entry stays"))

    def test_the_render_is_idempotent_and_check_agrees(self) -> None:
        self.land("a.md", "## A (2026-01-01)\n\nbody\n")
        shell(self.root, "concat-rebase-notes.sh", "--write")
        before = (self.root / "docs/rebase-notes.md").read_text()
        shell(self.root, "concat-rebase-notes.sh", "--write")
        self.assertEqual((self.root / "docs/rebase-notes.md").read_text(), before)
        self.assertEqual(shell(self.root, "concat-rebase-notes.sh", "--check").returncode, 0)

    def test_a_stale_render_fails_the_check(self) -> None:
        self.land("a.md", "## A (2026-01-01)\n\nbody\n")
        shell(self.root, "concat-rebase-notes.sh", "--write")
        self.land("b.md", "## B (2026-01-02)\n\nbody\n")
        result = shell(self.root, "concat-rebase-notes.sh", "--check")
        self.assertEqual(result.returncode, 1)
        self.assertIn("make docs-render", result.stderr)

    def test_a_fragment_without_its_heading_is_refused(self) -> None:
        self.land("bad.md", "no heading here\n")
        for flag in ("--lint", "--write"):
            with self.subTest(flag=flag):
                result = shell(self.root, "concat-rebase-notes.sh", flag)
                self.assertEqual(result.returncode, 1)
                self.assertIn("bad.md", result.stderr)

    def test_lint_does_not_read_the_rendered_file(self) -> None:
        """A pull request runs --lint on a tree whose docs/rebase-notes.md is not rendered."""
        self.land("a.md", "## A (2026-01-01)\n\nbody\n")
        self.assertEqual(shell(self.root, "concat-rebase-notes.sh", "--lint").returncode, 0)


class AdrIndexOrder(Fixture):
    def setUp(self) -> None:
        super().setUp()
        adr = self.root / "docs/adr"
        self.frag = adr / "_index_fragments"
        self.frag.mkdir(parents=True)
        (self.frag / "_header.md").write_text("# index\n\n")
        for slug in ("0001-a", "0002-b"):
            self.add_adr(slug)
        (self.frag / "_order.txt").write_text("0002-b\n0001-a\n")
        self.commit("legacy")

    def add_adr(self, slug: str) -> None:
        (self.root / "docs/adr" / f"{slug}.md").write_text(f"# ADR-{slug[:4]}\n")
        (self.frag / f"{slug}.md").write_text(f"| [ADR-{slug[:4]}]({slug}.md) | x |\n")

    def rows(self) -> list[str]:
        result = shell(self.root, "concat-adr-index.sh")
        self.assertEqual(result.returncode, 0, result.stderr)
        return [
            line.split("]")[0].split("[")[1]
            for line in stdout_text(result).splitlines()
            if "|" in line
        ]

    def test_the_frozen_manifest_comes_first_then_fragments_in_landing_order(self) -> None:
        self.add_adr("0050-late-number-lands-first")
        self.commit("lands first")
        self.add_adr("0010-early-number-lands-second")
        self.commit("lands second")
        self.assertEqual(self.rows(), [f"ADR-{n}" for n in ("0002", "0001", "0050", "0010")])

    def test_a_pull_request_names_no_position(self) -> None:
        """Adding the fragment is enough: the manifest is not edited and the row appears."""
        self.add_adr("0003-c")
        self.assertEqual((self.frag / "_order.txt").read_text(), "0002-b\n0001-a\n")
        self.assertEqual(self.rows()[-1], "ADR-" + "0003")


class PullRequestConflicts(Fixture):
    """GitHub merges without the union driver: only inputs avoid a conflict."""

    def setUp(self) -> None:
        super().setUp()
        (self.root / "CHANGELOG.md").write_text(
            "# Change Log\n\n## [Unreleased]\n\n## [1.0.0] - x\n"
        )
        (self.root / "changelog.d/added").mkdir(parents=True)
        (self.root / "changelog.d/added/.keep").write_text("")
        self.commit("changelog")

    def branch_from_master(self, name: str) -> None:
        git(self.root, "checkout", "-q", "-b", name, "master")

    def merge_clean(self, branch: str) -> bool:
        return (
            git(self.root, "merge-tree", "--write-tree", "master", branch, check=False).returncode
            == 0
        )

    def test_fragment_only_pull_requests_do_not_conflict_with_a_moved_master(self) -> None:
        self.branch_from_master("pr-a")
        (self.root / "changelog.d/added/a.md").write_text("- a\n")
        (self.notes / "a.md").write_text("## A (2026-01-01)\n\na\n")
        self.commit("pr a")
        git(self.root, "checkout", "-q", "master")
        self.branch_from_master("pr-b")
        (self.root / "changelog.d/added/b.md").write_text("- b\n")
        (self.notes / "b.md").write_text("## B (2026-01-02)\n\nb\n")
        self.commit("pr b")
        git(self.root, "checkout", "-q", "master")
        git(self.root, "merge", "-q", "--ff-only", "pr-a")
        shell(self.root, "concat-rebase-notes.sh", "--write")
        self.commit("chore(docs): render generated changelog and ADR index")
        self.assertTrue(self.merge_clean("pr-b"))

    def test_planted_a_pull_request_that_edits_the_rendered_file_conflicts(self) -> None:
        """The defect the decision removes: both sides edit the Unreleased block."""
        self.branch_from_master("pr-a")
        (self.root / "CHANGELOG.md").write_text(
            "# Change Log\n\n## [Unreleased]\n- a\n\n## [1.0.0] - x\n"
        )
        self.commit("pr a edits the render")
        git(self.root, "checkout", "-q", "master")
        self.branch_from_master("pr-b")
        (self.root / "CHANGELOG.md").write_text(
            "# Change Log\n\n## [Unreleased]\n- b\n\n## [1.0.0] - x\n"
        )
        self.commit("pr b edits the render")
        git(self.root, "checkout", "-q", "master")
        git(self.root, "merge", "-q", "--ff-only", "pr-a")
        self.assertFalse(self.merge_clean("pr-b"))


if __name__ == "__main__":
    unittest.main()
