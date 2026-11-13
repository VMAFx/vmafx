#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Pre-push ``--merge-base`` coverage for ``scripts/ci/check-dco.py`` (ADR-2462).

The commit-msg hook only sees commits made in the local checkout. A review
bot's commit pulled from the pull request branch reached CI without a sign-off
on #3426; the ``dco-sign-off`` pre-push hook checks every outgoing commit.
"""

from __future__ import annotations

import os
import shlex
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import yaml  # type: ignore[import-untyped]  # hook environments omit PyYAML stubs

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci.tests import check_dco_fixture as fx  # noqa: E402
from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

REVIEW_BOT = ("coderabbitai[bot]", "136622811+coderabbitai[bot]@users.noreply.github.com")
HOOK_ENTRY = ["python3", "scripts/ci/check-dco.py", "--merge-base", "origin/master"]


def push_gate(
    repo: fx.Repo, *args: str, to_ref: str | None = None, head_sha: str | None = None
) -> TextCommandResult:
    """Run the gate as the pre-push hook does: no ``--base``/``--head``."""
    env = fx.clean_env()
    if to_ref is not None:
        env["PRE_COMMIT_TO_REF"] = to_ref
    if head_sha is not None:
        env["HEAD_SHA"] = head_sha
    return run(
        [sys.executable, "-I", str(fx.GATE), *args],
        allowed_executables=(sys.executable,),
        cwd=repo.path,
        env=env,
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=fx.SCRIPT_TIMEOUT_S,
    )


class DcoPrePushTest(unittest.TestCase):
    """``--merge-base REF`` checks the outgoing commits not reachable from REF."""

    def setUp(self) -> None:
        """Create a repository with a ``feature`` branch forked from ``main``."""
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.repo = fx.Repo(Path(tmp.name))
        self.repo.git(["checkout", "-q", "-b", "feature"])

    def advance_main(self, *, signoff: bool) -> str:
        """Add one commit to ``main`` after the fork point and return to ``feature``."""
        self.repo.git(["checkout", "-q", "main"])
        rev = self.repo.commit("chore: main moves on", fx.HUMAN, signoff=signoff)
        self.repo.git(["checkout", "-q", "feature"])
        return rev

    def test_positive_signed_branch_passes(self) -> None:
        """Every outgoing commit carries a matching sign-off."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        self.repo.commit("fix: two", fx.HUMAN, signoff=True)
        self.advance_main(signoff=True)
        done = push_gate(self.repo, "--merge-base", "main")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("PASS - 2 commit(s)", done.stdout)

    def test_negative_review_bot_commit_under_signed_tip(self) -> None:
        """#3426 shape: an unsigned bot commit sits between signed commits."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        bot = self.repo.commit("fix: apply review suggestion", REVIEW_BOT)
        self.repo.commit("fix: three", fx.HUMAN, signoff=True)
        done = push_gate(self.repo, "--merge-base", "main")
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn(bot[:10], done.stdout)
        self.assertIn("Signed-off-by", done.stdout)

    def test_boundary_pre_commit_to_ref_selects_the_pushed_commit(self) -> None:
        """pre-commit exports the pushed commit; later local commits stay out."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        pushed = self.repo.commit("fix: two", fx.HUMAN, signoff=True)
        self.repo.commit("wip: local only", fx.HUMAN)
        done = push_gate(self.repo, "--merge-base", "main", to_ref=pushed)
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("PASS - 2 commit(s)", done.stdout)
        self.assertEqual(push_gate(self.repo, "--merge-base", "main").returncode, 1)

    def test_boundary_explicit_head_wins_over_pre_commit_to_ref(self) -> None:
        """``--head`` names the outgoing commit even inside a hook run."""
        signed = self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        unsigned = self.repo.commit("fix: two", fx.HUMAN)
        done = push_gate(self.repo, "--merge-base", "main", "--head", unsigned, to_ref=signed)
        self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
        self.assertIn(unsigned[:10], done.stdout)

    def test_negative_inherited_head_sha_does_not_replace_the_pushed_commit(self) -> None:
        """An exported ``HEAD_SHA`` naming a signed ancestor must not hide the pushed tip."""
        signed = self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        unsigned = self.repo.commit("fix: two", fx.HUMAN)
        for name, to_ref in (("hook run", unsigned), ("no hook variable", None)):
            with self.subTest(name):
                done = push_gate(self.repo, "--merge-base", "main", to_ref=to_ref, head_sha=signed)
                self.assertEqual(done.returncode, 1, done.stdout + done.stderr)
                self.assertIn(unsigned[:10], done.stdout)

    def test_boundary_commits_already_on_ref_are_not_checked(self) -> None:
        """After merging REF in, only the branch's own commits are outgoing."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        self.advance_main(signoff=False)
        self.repo.git(["merge", "-q", "--no-edit", "--signoff", "main"])
        done = push_gate(self.repo, "--merge-base", "main")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("PASS - 1 commit(s)", done.stdout)
        fork_point = push_gate(self.repo, "--base", self.repo.base, "--head", "HEAD")
        self.assertEqual(fork_point.returncode, 1, "the fork-point range must see main's commit")

    def test_negative_usage_errors_exit_two(self) -> None:
        """Conflicting modes and unresolvable revisions are usage errors."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        cases = {
            "with --base": (("--merge-base", "main", "--base", self.repo.base), None),
            "with --landed": (("--merge-base", "main", "--landed"), None),
            "unknown REF": (("--merge-base", "no/such/ref"), None),
            "unknown pushed commit": (("--merge-base", "main"), "0" * 40),
        }
        for name, (args, to_ref) in cases.items():
            with self.subTest(name):
                done = push_gate(self.repo, *args, to_ref=to_ref)
                self.assertEqual(done.returncode, 2, done.stdout + done.stderr)

    def test_negative_no_range_at_all_exits_two(self) -> None:
        """Without ``--merge-base`` the gate still needs ``--base`` and ``--head``."""
        done = push_gate(self.repo)
        self.assertEqual(done.returncode, 2, done.stdout + done.stderr)
        self.assertIn("--merge-base", done.stderr)

    def test_boundary_caller_push_variable_does_not_reach_the_gate(self) -> None:
        """These tests run inside pushes; the caller's pushed commit is not in the fixture."""
        self.repo.commit("fix: one", fx.HUMAN, signoff=True)
        with mock.patch.dict(os.environ, {"PRE_COMMIT_TO_REF": "f" * 40}):
            self.assertNotIn("PRE_COMMIT_TO_REF", fx.clean_env())
            done = push_gate(self.repo, "--merge-base", "main")
        self.assertEqual(done.returncode, 0, done.stdout + done.stderr)
        self.assertIn("PASS - 1 commit(s)", done.stdout)


class DcoPrePushHookContractTest(unittest.TestCase):
    """The pre-push hook runs the gate on every push with no filename filter."""

    def test_hook_runs_merge_base_mode_on_every_push(self) -> None:
        """Pin the stage, entry and unconditional run of ``dco-sign-off``."""
        config = yaml.safe_load((ROOT / ".pre-commit-config.yaml").read_text(encoding="utf-8"))
        hooks = {hook["id"]: hook for repo in config["repos"] for hook in repo["hooks"]}
        hook = hooks["dco-sign-off"]
        self.assertEqual(hook["stages"], ["pre-push"])
        self.assertEqual(shlex.split(hook["entry"]), HOOK_ENTRY)
        self.assertEqual(hook["language"], "system")
        self.assertIs(hook["always_run"], True)
        self.assertIs(hook["pass_filenames"], False)


if __name__ == "__main__":
    unittest.main()
