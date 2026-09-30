#!/usr/bin/env python3
# scripts/dev/test-resolve-state-md-conflict.py — regression test for the
# docs/state.md three-way conflict resolver.
#
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
#
"""Drive the docs/state.md resolver through real ``git rebase`` conflicts.

Every case builds a throwaway repository, commits a small ledger on master and
on a branch, runs a real ``git rebase master`` until git stops on
``docs/state.md``, and runs the resolver the way a developer does. Each
resolved file must pass ``scripts/ci/check-state-md-rows.sh``.

Both sides always add an ``_Updated`` line at the top, which is what makes git
stop: two insertions at the same point.

Usage:
    python3 scripts/dev/test-resolve-state-md-conflict.py [-v]
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / "scripts" / "dev" / "resolve-state-md-conflict.py"
CHECKER = ROOT / "scripts" / "ci" / "check-state-md-rows.sh"
GIT = shutil.which("git") or "git"
BASH = shutil.which("bash") or "bash"
TIMEOUT_SECONDS = 60
MAX_REBASE_STEPS = 10
STATE = "docs/state.md"

BASE = """<!-- markdownlint-disable MD013 -->
_Updated: 2026-09-01 (base entry)._

# Fork bug-status — `docs/state.md`

## First-release phase classification

| Disposition | Ledger rows | Release handling |
| --- | --- | --- |
| **RC2 stabilisation** | `T-FIX-A-2026-09-01`<br>`T-FIX-B-2026-09-01` | Fixes for the next candidate. |
| **RC3 performance** | `T-PERF-A-2026-09-01`<br>`T-PERF-B-2026-09-01` | RC3 owns these. |
| **RC4 training** | `T-TRAIN-A-2026-09-01` | Training rows. |
| **Explicitly deferred** | `T-DEFER-A-2026-09-01` | Deferred with a trigger. |

## Open bugs

| **T-OPEN-A-2026-09-01** — first open bug | `repro a` | open |
| **T-OPEN-B-2026-09-01** — second open bug | `repro b` | open |
| **T-OPEN-C-2026-09-01** — third open bug | `repro c` | open |
| **T-OPEN-D-2026-09-01** — fourth open bug | `repro d` | open |

<!-- T-OLD-2026-08-01 moved to Recently closed — fixed by #1 -->

## Recently closed

| **T-CLOSED-A-2026-08-20** — closed earlier | **FIXED** by #10. |
| **T-CLOSED-B-2026-08-10** — closed long ago | **FIXED** by #9. |
| **T-OLD-2026-08-01** — the tombstoned bug | **FIXED** by #1. |

## Update protocol

Move the row; never leave it behind.
"""

TOP = "<!-- markdownlint-disable MD013 -->\n"
CLOSED = "## Recently closed\n\n"


def row(bug: str, text: str, status: str) -> str:
    """One ledger row line."""
    return f"| **{bug}** — {text} | {status} |\n"


def open_row(bug: str) -> str:
    """The base fixture's Open row for T-OPEN-<bug>-2026-09-01."""
    ordinal = {"A": "first", "B": "second", "C": "third", "D": "fourth"}[bug]
    return f"| **T-OPEN-{bug}-2026-09-01** — {ordinal} open bug | `repro {bug.lower()}` | open |\n"


def edit(text: str, *pairs: tuple[str, str]) -> str:
    """Apply exact, single-occurrence replacements; fail loudly on a miss."""
    for old, new in pairs:
        if text.count(old) != 1:
            raise AssertionError(f"fixture edit target not unique: {old!r}")
        text = text.replace(old, new)
    return text


def updated(text: str, entry: str) -> str:
    """Add an `_Updated` line at the top, as every ledger-touching PR does."""
    return edit(text, (TOP, f"{TOP}_Updated: 2026-09-30 ({entry})._\n"))


def close(text: str, bug: str, note: str, tombstone: bool = False) -> str:
    """Move T-OPEN-<bug> from Open to the top of Recently closed."""
    bug_id = f"T-OPEN-{bug}-2026-09-01"
    stone = f"<!-- {bug_id} moved to Recently closed — {note} -->\n" if tombstone else ""
    closed = row(bug_id, f"closed by {note}", f"**FIXED** by {note}.")
    return edit(text, (open_row(bug), stone), (CLOSED, CLOSED + closed))


def disposition_line(text: str, label: str) -> str:
    """The disposition row of `text` labelled `label`."""
    rows = [line for line in text.splitlines() if line.startswith(f"| **{label}** |")]
    if len(rows) != 1:
        raise AssertionError(f"{len(rows)} rows labelled {label!r}")
    return rows[0]


def listed(text: str, label: str) -> list[str]:
    """The bug ids a disposition row lists, in order, short names only."""
    cell = disposition_line(text, label).split("|")[2]
    return [item.strip().strip("`").removesuffix("-2026-09-01") for item in cell.split("<br>")]


def relist(text: str, label: str, ids: list[str]) -> str:
    """Rewrite the id list of one disposition row (short names, fixture date)."""
    old = disposition_line(text, label)
    cells = old.split("|")
    cells[2] = " " + "<br>".join(f"`{bug}-2026-09-01`" for bug in ids) + " "
    return edit(text, (old, "|".join(cells)))


def clean_environment() -> dict[str, str]:
    """The caller's environment without any Git state, so no real repo is touched."""
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update(
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_EDITOR="true",
        LC_ALL="C",
    )
    return env


class Repo:
    """A disposable repository holding only docs/state.md."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.env = clean_environment()
        self.git("init", "-q", "-b", "master")
        for key, value in (
            ("user.name", "Resolver Test"),
            ("user.email", "fixture@example.invalid"),
            ("commit.gpgsign", "false"),
            ("core.autocrlf", "false"),
        ):
            self.git("config", key, value)
        (path / "docs").mkdir()

    def run(self, argv: list[str]) -> subprocess.CompletedProcess[str]:
        """Run a command in the repository with the scrubbed environment."""
        return subprocess.run(  # noqa: S603 -- fixed test argv, disposable repository
            argv,
            cwd=self.path,
            env=self.env,
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )

    def git(self, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
        """Run git; raise with its output when `check` and it fails."""
        proc = self.run([GIT, *args])
        if check and proc.returncode != 0:
            raise AssertionError(f"git {' '.join(args)} failed:\n{proc.stdout}{proc.stderr}")
        return proc

    def commit(self, text: str, message: str) -> None:
        """Commit `text` as docs/state.md."""
        (self.path / STATE).write_bytes(text.encode("utf-8"))
        self.git("add", STATE)
        self.git("commit", "-q", "-m", message)

    def resolver(self, *extra: str) -> subprocess.CompletedProcess[str]:
        """Run the resolver as a developer would, from the repository root."""
        return self.run([sys.executable, str(SCRIPT), STATE, *extra])

    def conflicted(self) -> bool:
        """True while git holds unmerged stages for docs/state.md."""
        return bool(self.git("ls-files", "-u", "--", STATE).stdout.strip())

    def text(self) -> str:
        """The working-tree docs/state.md."""
        return (self.path / STATE).read_bytes().decode("utf-8")


def build(path: Path, master: list[str], branch: list[str]) -> Repo:
    """Commit BASE, the branch's commits, then master's; check out the branch."""
    repo = Repo(path)
    repo.commit(BASE, "base")
    repo.git("checkout", "-q", "-b", "branch")
    for number, text in enumerate(branch):
        repo.commit(text, f"branch {number}")
    repo.git("checkout", "-q", "master")
    for number, text in enumerate(master):
        repo.commit(text, f"master {number}")
    repo.git("checkout", "-q", "branch")
    return repo


class ResolverCase(unittest.TestCase):
    """Shared helpers: a fresh repository per test and a rebase driver."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory(prefix="state-md-resolver-")
        self.addCleanup(self.tmp.cleanup)

    def start(self, master: list[str], branch: list[str]) -> Repo:
        """Build the repository and start a rebase that must stop on state.md."""
        repo = build(Path(self.tmp.name), master, branch)
        proc = repo.git("rebase", "master", check=False)
        self.assertNotEqual(proc.returncode, 0, "fixture did not conflict")
        self.assertTrue(repo.conflicted(), proc.stdout + proc.stderr)
        return repo

    def assert_resolves(self, repo: Repo, *extra: str) -> str:
        """Run the resolver; it must succeed, write LF only and pass the gate."""
        proc = repo.resolver(*extra)
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertIn("check-state-md-rows: OK", proc.stdout)
        raw = (repo.path / STATE).read_bytes()
        self.assertNotIn(b"\r", raw)
        self.assert_gate(repo)
        return raw.decode("utf-8")

    def assert_gate(self, repo: Repo) -> None:
        """Run check-state-md-rows.sh on the working-tree file independently."""
        proc = subprocess.run(  # noqa: S603 -- fixed bash running the in-repo gate
            [BASH, str(CHECKER), str(repo.path / STATE)],
            capture_output=True,
            text=True,
            timeout=TIMEOUT_SECONDS,
            check=False,
        )
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)

    def finish(self, repo: Repo) -> str:
        """Resolve every stop of the rebase with the tool; return the result."""
        for _ in range(MAX_REBASE_STEPS):
            if repo.conflicted():
                self.assert_resolves(repo)
                repo.git("add", STATE)
            proc = repo.git("rebase", "--continue", check=False)
            if proc.returncode == 0 and not (repo.path / ".git" / "rebase-merge").exists():
                self.assert_gate(repo)
                return repo.text()
        raise AssertionError("rebase did not finish")


class BranchMoves(ResolverCase):
    """Cases the old ours-wins resolver got wrong."""

    def test_a_branch_closes_a_bug_master_keeps_open_copy(self) -> None:
        master = edit(
            updated(BASE, "master"),
            (open_row("A"), open_row("A").replace("first open bug", "first bug, reworded")),
        )
        branch = close(updated(BASE, "branch closes B"), "B", "the branch")
        text = self.finish(self.start([master], [branch]))
        self.assertEqual(text.count("T-OPEN-B-2026-09-01"), 1)
        open_part = text.split("## Open bugs", 1)[1].split("## Recently closed", 1)[0]
        self.assertNotIn("T-OPEN-B-2026-09-01", open_part)
        self.assertIn("closed by the branch", text)
        self.assertIn("first bug, reworded", text)

    def test_b_later_branch_commit_rewrites_an_earlier_row(self) -> None:
        master = edit(updated(BASE, "master"), (open_row("D"), ""))
        first = close(updated(BASE, "branch v1"), "B", "branch v1")
        second = edit(
            first,
            ("_Updated: 2026-09-30 (branch v1)._", "_Updated: 2026-09-30 (branch v2)._"),
            ("closed by branch v1", "closed by branch v2"),
            ("**FIXED** by branch v1.", "**FIXED** by branch v2."),
        )
        text = self.finish(self.start([master], [first, second]))
        self.assertIn("closed by branch v2", text)
        self.assertNotIn("branch v1", text)
        self.assertEqual(text.count("T-OPEN-B-2026-09-01"), 1)
        self.assertNotIn("T-OPEN-D-2026-09-01", text)

    def test_c_both_sides_add_updated_lines_and_closed_rows(self) -> None:
        master = close(updated(BASE, "master closes C"), "C", "master")
        branch = close(updated(BASE, "branch closes D"), "D", "the branch")
        text = self.finish(self.start([master], [branch]))
        lines = text.splitlines()
        self.assertLess(
            lines.index("_Updated: 2026-09-30 (master closes C)._"),
            lines.index("_Updated: 2026-09-30 (branch closes D)._"),
        )
        closed = text.split(CLOSED, 1)[1].splitlines()
        self.assertTrue(closed[0].startswith("| **T-OPEN-C-2026-09-01**"), closed[0])
        self.assertTrue(closed[1].startswith("| **T-OPEN-D-2026-09-01**"), closed[1])
        self.assertTrue(closed[2].startswith("| **T-CLOSED-A-2026-08-20**"), closed[2])

    def test_stacked_branch_replays_a_commit_master_squashed(self) -> None:
        stacked = close(updated(BASE, "stacked PR closes C"), "C", "the stacked PR")
        prose = "Move the row; never leave it behind."
        squashed = edit(stacked, (prose, prose + " Squashed."))
        master = [squashed, updated(squashed, "master, later")]
        own = close(updated(stacked, "branch closes D"), "D", "the branch")
        text = self.finish(self.start(master, [stacked, own]))
        self.assertEqual(text.count("_Updated: 2026-09-30 (stacked PR closes C)._"), 1)
        self.assertEqual(text.count("| **T-OPEN-C-2026-09-01**"), 1)
        self.assertIn("closed by the branch", text)
        self.assertIn(prose + " Squashed.", text)

    def test_one_side_deletions(self) -> None:
        master = edit(updated(BASE, "master drops C"), (open_row("C"), ""))
        branch = edit(
            updated(BASE, "branch drops D and a closed row"),
            (open_row("D"), ""),
            (row("T-CLOSED-B-2026-08-10", "closed long ago", "**FIXED** by #9."), ""),
        )
        text = self.finish(self.start([master], [branch]))
        for gone in ("T-OPEN-C-2026-09-01", "T-OPEN-D-2026-09-01", "T-CLOSED-B-2026-08-10"):
            self.assertNotIn(gone, text)
        self.assertIn("T-OPEN-A-2026-09-01", text)


class Tombstones(ResolverCase):
    """Move tombstones are keyed by bug id, like rows."""

    def test_both_sides_close_with_tombstones_then_branch_rewords(self) -> None:
        master = close(updated(BASE, "master closes C"), "C", "master", tombstone=True)
        first = close(updated(BASE, "branch closes B"), "B", "the branch", tombstone=True)
        second = edit(
            first,
            ("_Updated: 2026-09-30 (branch closes B)._", "_Updated: 2026-09-30 (B, reworded)._"),
            (
                "Recently closed — the branch -->",
                "Recently closed — fixed by the reworded commit -->",
            ),
        )
        text = self.finish(self.start([master], [first, second]))
        open_part = text.split("## Open bugs", 1)[1].split("## Recently closed", 1)[0]
        for bug in ("B", "C"):
            self.assertNotIn(f"| **T-OPEN-{bug}-2026-09-01**", open_part)
            self.assertIn(f"<!-- T-OPEN-{bug}-2026-09-01 moved to Recently closed", open_part)
        self.assertIn("fixed by the reworded commit", text)
        self.assertNotIn("Recently closed — the branch -->", text)


class BothChanged(ResolverCase):
    """Rows both sides changed differently stop the tool until --take settles them."""

    def test_master_closed_while_branch_edited(self) -> None:
        master = close(updated(BASE, "master closes A"), "A", "master", tombstone=True)
        branch = edit(
            updated(BASE, "branch edits A"),
            (open_row("A"), open_row("A").replace("first open bug", "first bug, edited")),
        )
        repo = self.start([master], [branch])
        before = (repo.path / STATE).read_bytes()
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("T-OPEN-A-2026-09-01", proc.stderr)
        self.assertIn("--take T-OPEN-A-2026-09-01=ours", proc.stderr)
        self.assertEqual((repo.path / STATE).read_bytes(), before)

        text = self.assert_resolves(repo, "--take", "T-OPEN-A-2026-09-01=theirs")
        self.assertIn("first bug, edited", text)
        self.assertNotIn("T-OPEN-A-2026-09-01 moved to Recently closed", text)

        text = self.assert_resolves(repo, "--take", "T-OPEN-A-2026-09-01=ours")
        self.assertNotIn("first bug, edited", text)
        self.assertIn("closed by master", text)
        self.assertIn("T-OPEN-A-2026-09-01 moved to Recently closed", text)


class Dispositions(ResolverCase):
    """Disposition rows are records keyed by label; the id list merges as a set."""

    RC2 = "RC2 stabilisation"
    RC3 = "RC3 performance"
    RC4 = "RC4 training"
    DEFER = "Explicitly deferred"

    def test_both_sides_add_different_ids_to_one_row(self) -> None:
        master = relist(updated(BASE, "master"), self.RC3, ["T-PERF-A", "T-PERF-B", "T-PERF-C"])
        branch = relist(updated(BASE, "branch"), self.RC3, ["T-PERF-D", "T-PERF-A", "T-PERF-B"])
        branch = relist(branch, self.DEFER, ["T-DEFER-A", "T-DEFER-B"])
        text = self.finish(self.start([master], [branch]))
        self.assertEqual(listed(text, self.RC3), ["T-PERF-A", "T-PERF-B", "T-PERF-C", "T-PERF-D"])
        self.assertEqual(listed(text, self.DEFER), ["T-DEFER-A", "T-DEFER-B"])

    def test_one_side_removes_an_id(self) -> None:
        master = relist(updated(BASE, "master"), self.RC3, ["T-PERF-B"])
        branch = relist(updated(BASE, "branch"), self.RC3, ["T-PERF-A", "T-PERF-B", "T-PERF-D"])
        text = self.finish(self.start([master], [branch]))
        self.assertEqual(listed(text, self.RC3), ["T-PERF-B", "T-PERF-D"])

    def test_duplicate_same_label_rows_on_ours_collapse(self) -> None:
        master = updated(BASE, "master")
        row = disposition_line(master, self.RC3)
        extra = row.replace("B-2026-09-01`", "B-2026-09-01`<br>`T-PERF-C-2026-09-01`")
        master = edit(master, (row + "\n", f"{row}\n{row}\n{extra}\n"))
        branch = relist(updated(BASE, "branch"), self.RC3, ["T-PERF-A", "T-PERF-B", "T-PERF-D"])
        repo = self.start([master], [branch])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertEqual(proc.stdout.count("collapsed the duplicate 'RC3 performance' row"), 2)
        text = repo.text()
        self.assertEqual(listed(text, self.RC3), ["T-PERF-A", "T-PERF-B", "T-PERF-C", "T-PERF-D"])
        repo.git("add", STATE)
        self.assertEqual(listed(self.finish(repo), self.RC3)[-1], "T-PERF-D")

    def test_id_moved_from_rc2_to_rc3_on_one_side(self) -> None:
        master = relist(updated(BASE, "master"), self.RC3, ["T-PERF-A", "T-PERF-B", "T-PERF-C"])
        branch = relist(updated(BASE, "branch"), self.RC2, ["T-FIX-B"])
        branch = relist(branch, self.RC3, ["T-PERF-A", "T-PERF-B", "T-FIX-A"])
        repo = self.start([master], [branch])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 0, proc.stdout + proc.stderr)
        self.assertNotIn("WARNING", proc.stderr)
        text = repo.text()
        self.assertEqual(listed(text, self.RC2), ["T-FIX-B"])
        self.assertEqual(listed(text, self.RC3), ["T-PERF-A", "T-PERF-B", "T-PERF-C", "T-FIX-A"])

    def test_id_landing_in_two_rows_is_flagged(self) -> None:
        master = relist(updated(BASE, "master"), self.RC3, ["T-PERF-B"])
        master = relist(master, self.DEFER, ["T-DEFER-A", "T-PERF-A"])
        branch = relist(updated(BASE, "branch"), self.RC3, ["T-PERF-B"])
        branch = relist(branch, self.RC4, ["T-TRAIN-A", "T-PERF-A"])
        repo = self.start([master], [branch])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("WARNING: T-PERF-A-2026-09-01 is listed in 2 disposition rows", proc.stderr)
        self.assertIn("T-PERF-A", listed(repo.text(), self.RC4))

    def test_prose_cell_edited_on_both_sides_needs_a_take(self) -> None:
        master = edit(updated(BASE, "master"), ("RC3 owns these.", "RC3 owns these, says master."))
        branch = edit(updated(BASE, "branch"), ("RC3 owns these.", "RC3 owns these, says branch."))
        branch = relist(branch, self.RC3, ["T-PERF-A", "T-PERF-B", "T-PERF-D"])
        repo = self.start([master], [branch])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        self.assertIn("RC3 performance (disposition row)", proc.stderr)
        text = self.assert_resolves(repo, "--take", "RC3 performance=theirs")
        self.assertIn("says branch", text)
        self.assertNotIn("says master", text)
        self.assertEqual(listed(text, self.RC3), ["T-PERF-A", "T-PERF-B", "T-PERF-D"])


class PlainLines(ResolverCase):
    """Lines without an id or label merge line by line."""

    def test_overlapping_deletions_are_not_a_conflict(self) -> None:
        stale = "_Updated: 2026-09-01 (base entry)._\n"
        master = edit(BASE, (stale + "\n", ""))
        branch = edit(BASE, (stale, ""), ("Move the row;", "Move the row, always;"))
        text = self.finish(self.start([master], [branch]))
        self.assertNotIn("base entry", text)
        self.assertIn(TOP + "# Fork bug-status", text)
        self.assertIn("Move the row, always;", text)

    def test_prose_line_edited_on_both_sides_needs_a_line_take(self) -> None:
        prose = "Move the row; never leave it behind."
        master = edit(updated(BASE, "master"), (prose, prose + " (master)"))
        branch = edit(updated(BASE, "branch"), (prose, prose + " (branch)"))
        repo = self.start([master], [branch])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 1, proc.stdout + proc.stderr)
        handle = next(
            word.split("=", 1)[0]
            for word in proc.stderr.split()
            if word.startswith("line:") and word.endswith("=theirs")
        )
        text = self.assert_resolves(repo, "--take", f"{handle}=theirs")
        self.assertIn(prose + " (branch)", text)
        self.assertNotIn(prose + " (master)", text)


class Refusals(ResolverCase):
    """Bad usage and states the tool must refuse without writing."""

    def test_not_mid_rebase(self) -> None:
        repo = build(Path(self.tmp.name), [updated(BASE, "master")], [])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
        self.assertIn("no unmerged index stages", proc.stderr)

    def test_bad_usage(self) -> None:
        repo = self.start([updated(BASE, "master")], [updated(BASE, "branch")])
        before = (repo.path / STATE).read_bytes()
        cases: list[list[str]] = [
            [],
            [STATE, "--take", "nonsense"],
            [STATE, "--take", "T-NOPE-2026-01-01=ours"],
        ]
        for argv in cases:
            with self.subTest(argv=argv):
                cmd = [sys.executable, str(SCRIPT), *argv]
                proc = repo.run(cmd)
                self.assertEqual(proc.returncode, 2, proc.stdout + proc.stderr)
                self.assertEqual((repo.path / STATE).read_bytes(), before)

    def test_gate_failure_is_loud(self) -> None:
        dup = row("T-CLOSED-A-2026-08-20", "closed earlier", "**FIXED** by #10.")
        master = edit(
            updated(BASE, "master"), (dup, dup + dup.replace("earlier", "earlier (copy)"))
        )
        repo = self.start([master], [updated(BASE, "branch")])
        proc = repo.resolver()
        self.assertEqual(proc.returncode, 3, proc.stdout + proc.stderr)
        self.assertIn("ROW GATE REJECTS", proc.stderr)


if __name__ == "__main__":
    unittest.main()
