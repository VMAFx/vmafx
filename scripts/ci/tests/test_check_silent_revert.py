#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the silent-revert gate (ADR-1284).

Every case builds a disposable repository, reproduces one merge shape, and
asserts what ``scripts/ci/check-silent-revert.py`` reports for it.  The suite is
positive / negative / boundary per HISS-15: a reproduced silent revert must be
caught, ordinary work must stay clean, and everything that makes the analysis
unsound must exit non-zero rather than print "clean".

``test_real_history_replay`` re-runs the gate against the pair that motivated
it — ``31a51afb2`` (#101, ADR-0759) and ``92ea978a4`` (#102), where an unrelated
CUDA change reset three HIP files to their pre-#101 blobs.  It skips when those
commits are not in the clone (a shallow checkout), because a skip is honest and
a silent pass is not.
"""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "scripts" / "ci" / "check-silent-revert.py"
GIT = shutil.which("git") or "/usr/bin/git"

# The historical pair the gate was built from; see
# .workingdir/evidence/silent-reverts-2026-09-18.md and ADR-1284.
ADR_0759_LANDED = "31a51afb2"
ADR_0759_REVERTED = "92ea978a4"


def git_env() -> dict[str, str]:
    """A git environment that cannot reach the developer's real configuration."""
    clean = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    clean.update(
        GIT_CONFIG_NOSYSTEM="1",
        GIT_CONFIG_GLOBAL=os.devnull,
        GIT_AUTHOR_NAME="Fixture",
        GIT_AUTHOR_EMAIL="fixture@example.invalid",
        GIT_COMMITTER_NAME="Fixture",
        GIT_COMMITTER_EMAIL="fixture@example.invalid",
        LC_ALL="C",
    )
    return clean


def git(repo: Path, *args: str, check: bool = True) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- fixed argv, no shell, disposable fixture repo
        [GIT, "-C", str(repo), *args],
        env=git_env(),
        text=True,
        capture_output=True,
        check=check,
    )


def run_gate(repo: Path, base: str, head: str, **env: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(  # noqa: S603 -- fixed argv, no shell, gate under test
        [sys.executable, str(GATE), "--repo", str(repo), "--base", base, "--head", head],
        env={**git_env(), **env},
        text=True,
        capture_output=True,
        check=False,
    )


FIXED = """int scale_of(int value)
{
    /* ADR-9999: the accumulator overflows int32 on bright 16-bit input. */
    int64_t accumulator = 0;
    accumulator += (int64_t)value * 4;
    return (int)(accumulator >> 2);
}
"""

STALE = """int scale_of(int value)
{
    int accumulator = 0;
    accumulator += value * 4;
    return accumulator >> 2;
}
"""


class SilentRevertGateTest(unittest.TestCase):
    """Each test owns one disposable repository."""

    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = Path(self.tmp.name) / "repo"
        self.repo.mkdir()
        git(self.repo, "init", "-q", "-b", "master")

    def write(self, path: str, text: str) -> None:
        target = self.repo / path
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def commit(self, message: str) -> str:
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", message)
        return git(self.repo, "rev-parse", "HEAD").stdout.strip()

    def seed(self) -> str:
        """Common history: a file in its stale form, plus an unrelated file."""
        self.write("core/src/scale.c", STALE)
        self.write("README.md", "# fixture\n")
        return self.commit("feat(core): add the scaler")

    # ---------- positive: the defect the gate exists for ----------

    def test_reproduced_silent_revert_is_caught(self) -> None:
        """A branch that carries the pre-fix copy of a file must fail the gate."""
        self.seed()
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        # The branch is rebased onto the fix — merge-base == base, so its own
        # diff and the merge result are the same thing.  Only history can show
        # that the file it ships is the pre-fix copy.  This is the shape
        # 92ea978a4 had.
        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/scale.c", STALE)
        self.write("core/src/unrelated.c", "void unrelated(void) { }\n")
        head = self.commit("perf(core): unrelated tweak")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("core/src/scale.c", result.stdout)
        self.assertIn("rewind", result.stdout)
        self.assertIn("reverse-hunk", result.stdout)
        self.assertIn(base[:9], result.stdout, "the report must name the commit being undone")

    def test_merge_resolution_dropping_target_lines_is_caught(self) -> None:
        """A merge commit inside the branch that resolves against the target fails."""
        self.seed()
        git(self.repo, "checkout", "-q", "-b", "feature")
        self.write("core/src/scale.c", STALE + "void branch_only(void) { }\n")
        self.commit("feat(core): branch-only helper")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        # Merge master into the branch and resolve by keeping the branch side:
        # the resolution lives in the merge commit, not in any commit of the
        # branch's own work, so it is not intent.
        git(self.repo, "checkout", "-q", "feature")
        git(self.repo, "merge", "--no-commit", "--no-ff", base, check=False)
        self.write("core/src/scale.c", STALE + "void branch_only(void) { }\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "merge master into feature")
        head = git(self.repo, "rev-parse", "HEAD").stdout.strip()

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("dropped", result.stdout)
        self.assertIn("int64_t accumulator", result.stdout)

    # ---------- negative: ordinary work must not be flagged ----------

    def test_ordinary_branch_is_clean(self) -> None:
        """Adding new work on top of the target reports nothing."""
        self.seed()
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/extra.c", "int extra(void) { return 7; }\n")
        head = self.commit("feat(core): add an extra helper")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("clean", result.stdout)

    def test_stale_branch_that_does_not_touch_the_file_is_clean(self) -> None:
        """Being behind the target is not by itself a revert."""
        start = self.seed()
        git(self.repo, "checkout", "-q", "-b", "feature", start)
        self.write("core/src/extra.c", "int extra(void) { return 7; }\n")
        head = self.commit("feat(core): add an extra helper")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_dropping_conflict_markers_is_clean(self) -> None:
        """Deleting markers the target committed must not read as a revert.

        Master really has carried them: ``0c494cca0`` left three in
        ``core/src/feature/cuda/integer_vif_cuda.c`` and the PR that removed
        them reset the file to its pre-marker blob.
        """
        self.seed()
        self.write(
            "core/src/scale.c", STALE.replace("{\n", "{\n<<<<<<< HEAD\n=======\n>>>>>>> x\n")
        )
        base = self.commit("chore(core): merge-train artefact")

        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/scale.c", STALE)
        head = self.commit("fix(core): drop stray conflict markers")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_generated_paths_are_exempt(self) -> None:
        """A rewound rendered file is a regeneration artefact, not a lost commit."""
        self.write("CHANGELOG.md", "# changelog\n\nold body\n")
        self.commit("chore: seed the changelog")
        self.write("CHANGELOG.md", "# changelog\n\nrendered body\nwith more text here\n")
        base = self.commit("chore(release): render the changelog")

        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("CHANGELOG.md", "# changelog\n\nold body\n")
        head = self.commit("chore(release): re-render the changelog")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_branch_deletion_is_not_a_reverse_hunk(self) -> None:
        """A file the branch deletes on purpose is not a partial rewind of it.

        The shape that used to fire is a target commit that only *added* the
        file: every line it wrote is live, the merge removes all of them, and
        it puts nothing in their place — which is precisely what ``_undone``
        asks of a pure-addition commit.  Every deliberate file removal in a
        branch therefore read as a reverse-hunk against whoever wrote it.
        """
        self.write("README.md", "# fixture\n")
        self.commit("chore: seed the fixture")

        self.write("testdata/retired_helper.py", FIXED)
        base = self.commit("test: add the retired helper (ADR-9997)")

        git(self.repo, "checkout", "-q", "-b", "feature", base)
        git(self.repo, "rm", "-q", "testdata/retired_helper.py")
        head = self.commit("chore(testdata): remove what ADR-9998 retired")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn("reverse-hunk", result.stdout)

    def test_deletion_the_branch_never_asked_for_is_still_caught(self) -> None:
        """Skipping deleted paths in reverse-hunk must not blind `dropped`."""
        self.seed()
        git(self.repo, "checkout", "-q", "-b", "feature")
        self.write("core/src/extra.c", "int extra(void) { return 7; }\n")
        self.commit("feat(core): add an extra helper")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        # The merge resolves by deleting a file only master wrote; no commit of
        # the branch's own work removed it.
        git(self.repo, "checkout", "-q", "feature")
        git(self.repo, "merge", "--no-commit", "--no-ff", base, check=False)
        (self.repo / "core" / "src" / "scale.c").unlink()
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "merge master into feature")
        head = git(self.repo, "rev-parse", "HEAD").stdout.strip()

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("dropped", result.stdout)
        self.assertIn("int64_t accumulator", result.stdout)

    def test_text_first_written_in_a_merge_is_not_a_resurrection(self) -> None:
        """New text a conflict resolution authors never stood on the target."""
        base = self.seed()
        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/extra.c", "int extra(void) { return 7; }\n")
        self.commit("feat(core): add an extra helper")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/extra.c", "int extra(void) { return 8; }\n")
        base = self.commit("feat(core): add a different extra helper")

        git(self.repo, "checkout", "-q", "feature")
        git(self.repo, "merge", "--no-commit", "--no-ff", base, check=False)
        # The resolution is neither side: it is text this merge invents, which
        # is why no non-merge commit of the branch carries it.
        self.write(
            "core/src/extra.c",
            "int extra(void) { return 7; }\nint reconciled_helper(void) { return 15; }\n",
        )
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "merge master into feature")
        head = git(self.repo, "rev-parse", "HEAD").stdout.strip()

        result = run_gate(self.repo, base, head)
        self.assertNotIn("resurrected", result.stdout)

    def test_text_the_target_deleted_coming_back_is_caught(self) -> None:
        """The other half of the definition still has to fire."""
        self.write("core/src/scale.c", FIXED)
        self.write("README.md", "# fixture\n")
        start = self.commit("feat(core): add the scaler")

        git(self.repo, "checkout", "-q", "-b", "feature", start)
        self.write("README.md", "# fixture\n\nbranch note about the fixture\n")
        self.commit("docs: note the fixture")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/scale.c", STALE)
        base = self.commit("revert(core): drop the int64 accumulator for now")

        git(self.repo, "checkout", "-q", "feature")
        git(self.repo, "merge", "--no-commit", "--no-ff", base, check=False)
        # The resolution brings back exactly what the target deleted.
        self.write("core/src/scale.c", FIXED)
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "merge master into feature")
        head = git(self.repo, "rev-parse", "HEAD").stdout.strip()

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("resurrected", result.stdout)
        self.assertIn("int64_t accumulator", result.stdout)

    # ---------- boundary: declaration and fail-closed ----------

    def test_declaration_turns_findings_into_notices(self) -> None:
        """`reverts: #N` in the body passes the same tree the gate just failed."""
        self.seed()
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")
        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/scale.c", STALE)
        head = self.commit("perf(core): unrelated tweak")

        self.assertEqual(run_gate(self.repo, base, head).returncode, 1)

        declared = run_gate(self.repo, base, head, PR_BODY="reverts: #4242 — the fix regressed")
        self.assertEqual(declared.returncode, 0, declared.stdout + declared.stderr)
        self.assertIn("::notice", declared.stdout)
        self.assertNotIn("::error", declared.stdout)

    def test_template_placeholder_is_not_a_declaration(self) -> None:
        """An unedited `intentional revert: REASON` placeholder must not exempt."""
        self.seed()
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")
        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/scale.c", STALE)
        head = self.commit("perf(core): unrelated tweak")

        result = run_gate(self.repo, base, head, PR_BODY="intentional revert: REASON")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)

    # ---------- boundary: the in-tree declared-reversal allowlist ----------

    def partial_rewind(self) -> tuple[str, str]:
        """A branch that undoes one commit's hunks but ships no historical blob."""
        self.seed()
        self.write("core/src/scale.c", FIXED)
        base = self.commit("fix(core): accumulate the scaler in int64 (ADR-9999)")

        git(self.repo, "checkout", "-q", "-b", "feature", base)
        self.write("core/src/scale.c", STALE + "void added_by_the_branch(void) { }\n")
        head = self.commit("refactor(core): take the scaler back to int arithmetic")
        return base, head

    def allowlist(self, text: str) -> None:
        target = self.repo / "scripts" / "ci" / "silent-revert-allowlist.json"
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_text(text, encoding="utf-8")

    def entry(self, **overrides: object) -> str:
        entry = {
            "adr": "ADR-9998",
            "kind": "reverse-hunk",
            "undoes": overrides.pop("undoes", ""),
            "evidence": "(?:accumulator|ADR-9999)",
            "reason": "ADR-9998 supersedes the int64 accumulator.",
            "paths": ["core/src/scale.c"],
        }
        entry.update(overrides)
        return json.dumps({"reversals": [entry]})

    def test_allowlist_entry_declares_its_own_finding(self) -> None:
        base, head = self.partial_rewind()
        self.assertEqual(run_gate(self.repo, base, head).returncode, 1)

        self.allowlist(self.entry(undoes=base))
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("ADR-9998", result.stdout)
        self.assertIn("::notice", result.stdout)
        self.assertNotIn("::error", result.stdout)

    def test_allowlist_entry_for_another_commit_does_not_declare(self) -> None:
        """`undoes` is what keeps an entry from becoming a path exclusion."""
        base, head = self.partial_rewind()
        self.allowlist(self.entry(undoes="0" * 40))
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("::error", result.stdout)
        self.assertNotIn("ADR-9998", result.stdout)

    def test_allowlist_entry_covering_only_some_evidence_does_not_declare(self) -> None:
        """One undeclared line in a finding leaves the whole finding blocking."""
        base, head = self.partial_rewind()
        self.allowlist(self.entry(undoes=base, evidence="ADR-9999"))
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("::error", result.stdout)
        self.assertNotIn("ADR-9998", result.stdout)

    def test_allowlist_entry_for_another_path_does_not_declare(self) -> None:
        base, head = self.partial_rewind()
        self.allowlist(self.entry(undoes=base, paths=["core/src/other.c"]))
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertNotIn("ADR-9998", result.stdout)

    def test_reverse_hunk_entry_without_a_commit_is_rejected(self) -> None:
        base, head = self.partial_rewind()
        self.allowlist(self.entry())
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no `undoes` commit", result.stdout)

    def test_unreadable_allowlist_fails_closed(self) -> None:
        base, head = self.partial_rewind()
        self.allowlist("{ this is not json")
        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertNotIn("clean", result.stdout)

    def test_unresolvable_ref_fails_closed(self) -> None:
        self.seed()
        result = run_gate(self.repo, "0000000000000000000000000000000000000000", "master")
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("::error", result.stdout)
        self.assertNotIn("clean", result.stdout)

    def test_unrelated_histories_fail_closed(self) -> None:
        """No merge base means the merge result cannot be established."""
        base = self.seed()
        git(self.repo, "checkout", "-q", "--orphan", "other")
        git(self.repo, "rm", "-rqf", ".")
        self.write("other.txt", "unrelated root\n")
        head = self.commit("chore: unrelated root")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("no merge base", result.stdout)

    def test_conflicting_merge_fails_closed(self) -> None:
        """An unmergeable pair is reported, never silently passed."""
        start = self.seed()
        git(self.repo, "checkout", "-q", "-b", "feature", start)
        self.write("core/src/scale.c", STALE.replace("value * 4", "value * 5"))
        head = self.commit("perf(core): branch edit")

        git(self.repo, "checkout", "-q", "master")
        self.write("core/src/scale.c", STALE.replace("value * 4", "value * 6"))
        base = self.commit("perf(core): master edit")

        result = run_gate(self.repo, base, head)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertIn("does not resolve cleanly", result.stdout)


class RealHistoryReplayTest(unittest.TestCase):
    """Replay the silent revert that motivated the gate, from this repo's history."""

    def test_real_history_replay(self) -> None:
        probe = subprocess.run(  # noqa: S603 -- fixed argv, no shell
            [GIT, "-C", str(ROOT), "cat-file", "-e", f"{ADR_0759_REVERTED}^{{commit}}"],
            env=git_env(),
            capture_output=True,
            text=True,
            check=False,
        )
        if probe.returncode != 0:
            self.skipTest(f"{ADR_0759_REVERTED} not in this clone (shallow checkout)")

        result = run_gate(ROOT, ADR_0759_LANDED, ADR_0759_REVERTED)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        for path in (
            "core/src/feature/hip/integer_adm_hip.c",
            "core/src/feature/hip/integer_adm/adm_cm.hip",
            "core/src/feature/hip/integer_adm/adm_csf.hip",
        ):
            self.assertIn(path, result.stdout)
        self.assertIn("the merge undoes 31a51afb2", result.stdout)
        self.assertIn("AdmBufferHip", result.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
