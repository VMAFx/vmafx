#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fixture coverage for the DCO sign-off gate (ADR-2462).

Every case builds a disposable repository, plants commits and asserts what
``scripts/ci/check-dco.py`` reports.  A check never seen failing is not a
check: ``test_planted_unsigned_commit_fails`` is the planted defect, and the
bot cases pin the exemption to the narrow shape the ADR states.  The last
cases read the repository's own wiring (workflow job, aggregator list,
Renovate preset, template) so a rename cannot silently disarm the gate.
"""

from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
GATE = ROOT / "scripts" / "ci" / "check-dco.py"
GIT = shutil.which("git") or "/usr/bin/git"
HUMAN = ("Ada Example", "ada@example.org")
BOT_LOGIN = "renovate[bot]"
BOT_IDENT = (BOT_LOGIN, "29139614+renovate[bot]@users.noreply.github.com")


def git_env(ident: tuple[str, str]) -> dict[str, str]:
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    env.update(
        GIT_AUTHOR_NAME=ident[0],
        GIT_AUTHOR_EMAIL=ident[1],
        GIT_COMMITTER_NAME=ident[0],
        GIT_COMMITTER_EMAIL=ident[1],
        GIT_CONFIG_GLOBAL="/dev/null",
        GIT_CONFIG_SYSTEM="/dev/null",
    )
    return env


def clean_env() -> dict[str, str]:
    """The environment without the variables the gate reads, so a CI run cannot leak into a case."""
    drop = (
        "BASE_SHA",
        "HEAD_SHA",
        "PR_AUTHOR",
        "PR_AUTHOR_TYPE",
        "PR_CREATED_AT",
        "DCO_RELEASE_PR",
    )
    return {k: v for k, v in os.environ.items() if k not in drop}


class Repo:
    """A throwaway repository with an initial commit on ``main``."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.git(["init", "-q", "-b", "main"])
        self.commit("initial", HUMAN, signoff=True)
        self.base = self.git(["rev-parse", "HEAD"]).strip()

    def git(self, args: list[str], ident: tuple[str, str] = HUMAN) -> str:
        done = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [GIT, *args],
            cwd=self.path,
            env=git_env(ident),
            capture_output=True,
            text=True,
            check=True,
        )
        return done.stdout

    def commit(self, message: str, ident: tuple[str, str], signoff: bool = False) -> str:
        n = len(list(self.path.glob("f*.txt")))
        (self.path / f"f{n}.txt").write_text(f"{n}\n")
        self.git(["add", "-A"], ident)
        flags = ["-s"] if signoff else []
        self.git(["commit", "-q", *flags, "-m", message], ident)
        return self.git(["rev-parse", "HEAD"], ident).strip()

    def gate(self, *extra: str) -> subprocess.CompletedProcess[str]:
        head = self.git(["rev-parse", "HEAD"]).strip()
        return subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [sys.executable, "-I", str(GATE), "--base", self.base, "--head", head, *extra],
            cwd=self.path,
            env=clean_env(),
            capture_output=True,
            text=True,
            check=False,
        )


class DcoGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Repo(Path(self._tmp.name))

    def test_signed_commit_passes(self) -> None:
        self.repo.commit("feat: signed", HUMAN, signoff=True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("PASS", res.stdout)

    def test_planted_unsigned_commit_fails(self) -> None:
        self.repo.commit("feat: unsigned", HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("::error title=DCO sign-off::", res.stdout)
        self.assertIn("feat: unsigned", res.stdout)

    def test_one_unsigned_among_signed_fails_and_names_only_it(self) -> None:
        self.repo.commit("feat: ok one", HUMAN, signoff=True)
        self.repo.commit("feat: missing", HUMAN)
        self.repo.commit("feat: ok two", HUMAN, signoff=True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1)
        self.assertIn("feat: missing", res.stdout)
        self.assertNotIn("feat: ok", res.stdout)

    def test_signoff_for_someone_else_fails(self) -> None:
        msg = "feat: other\n\nSigned-off-by: Bob Other <bob@example.org>"
        self.repo.commit(msg, HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1)
        self.assertIn("matches neither", res.stdout)

    def test_signoff_in_prose_not_in_trailer_block_fails(self) -> None:
        msg = "feat: prose\n\nSigned-off-by: Ada Example <ada@example.org>\n\nMore text after it."
        self.repo.commit(msg, HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout)

    def test_signoff_key_is_case_insensitive_and_email_match_too(self) -> None:
        msg = "feat: case\n\nsigned-off-by: Ada Example <ADA@Example.org>"
        self.repo.commit(msg, HUMAN)
        self.assertEqual(self.repo.gate().returncode, 0)

    def test_empty_range_passes(self) -> None:
        res = self.repo.gate()
        self.assertEqual(res.returncode, 0)
        self.assertIn("0 commit(s)", res.stdout)

    def test_merge_commit_is_not_judged(self) -> None:
        self.repo.git(["checkout", "-q", "-b", "side"])
        self.repo.commit("feat: side", HUMAN, signoff=True)
        self.repo.git(["checkout", "-q", "main"])
        self.repo.commit("feat: main", HUMAN, signoff=True)
        self.repo.base = self.repo.git(["rev-parse", "HEAD~1"]).strip()
        self.repo.git(["merge", "-q", "--no-ff", "-m", "Merge side", "side"])
        res = self.repo.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_bad_range_is_an_error_not_a_pass(self) -> None:
        res = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [sys.executable, "-I", str(GATE), "--base", "0" * 40, "--head", "HEAD"],
            cwd=self.repo.path,
            capture_output=True,
            text=True,
            check=False,
        )
        self.assertEqual(res.returncode, 2)

    def test_missing_arguments_is_usage_error(self) -> None:
        env = {k: v for k, v in os.environ.items() if k not in ("BASE_SHA", "HEAD_SHA")}
        res = subprocess.run(  # noqa: S603 -- fixed argv, no shell, fixture repo
            [sys.executable, "-I", str(GATE)], env=env, capture_output=True, text=True, check=False
        )
        self.assertEqual(res.returncode, 2)


class DcoBotExemptionTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Repo(Path(self._tmp.name))

    def test_bot_commit_in_bot_pull_request_is_exempt(self) -> None:
        self.repo.commit("chore(deps): bump", BOT_IDENT)
        res = self.repo.gate("--author", BOT_LOGIN, "--author-type", "Bot")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_human_commit_on_bot_branch_is_not_exempt(self) -> None:
        self.repo.commit("chore(deps): bump", BOT_IDENT)
        self.repo.commit("fix: hand edit", HUMAN)
        res = self.repo.gate("--author", BOT_LOGIN, "--author-type", "Bot")
        self.assertEqual(res.returncode, 1)
        self.assertIn("fix: hand edit", res.stdout)
        self.assertNotIn("chore(deps)", res.stdout)

    def test_bot_looking_commit_in_a_human_pull_request_is_not_exempt(self) -> None:
        self.repo.commit("chore: spoof", BOT_IDENT)
        res = self.repo.gate("--author", "ada", "--author-type", "User")
        self.assertEqual(res.returncode, 1)

    def test_user_account_named_like_a_bot_is_not_exempt(self) -> None:
        self.repo.commit("chore: spoof", BOT_IDENT)
        res = self.repo.gate("--author", BOT_LOGIN, "--author-type", "User")
        self.assertEqual(res.returncode, 1)

    def test_bot_not_on_the_list_is_not_exempt(self) -> None:
        ident = ("evil[bot]", "1+evil[bot]@users.noreply.github.com")
        self.repo.commit("chore: other bot", ident)
        res = self.repo.gate("--author", "evil[bot]", "--author-type", "Bot")
        self.assertEqual(res.returncode, 1)

    def test_bot_pr_with_commit_authored_by_another_bot_is_not_exempt(self) -> None:
        self.repo.commit(
            "chore: mixed", ("dependabot[bot]", "49699333+dependabot[bot]@users.noreply.github.com")
        )
        res = self.repo.gate("--author", BOT_LOGIN, "--author-type", "Bot")
        self.assertEqual(res.returncode, 1)

    def test_every_listed_bot_is_exempt_for_its_own_commits(self) -> None:
        spec = importlib.util.spec_from_file_location("check_dco", GATE)
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        sys.modules["check_dco"] = mod
        self.addCleanup(sys.modules.pop, "check_dco", None)
        spec.loader.exec_module(mod)
        for login in mod.BOT_LOGINS:
            commit = mod.Commit("a" * 40, f"1+{login}@users.noreply.github.com", "x@y", "s", "s")
            self.assertTrue(mod.is_exempt_bot_commit(commit, login, "Bot"), login)

    def test_release_pr_flag_exempts_everything(self) -> None:
        self.repo.commit("chore: release", HUMAN)
        res = self.repo.gate("--release-pr")
        self.assertEqual(res.returncode, 0)
        self.assertIn("release pull request", res.stdout)


class DcoCutoffTest(unittest.TestCase):
    """The rollout of ADR-2462: pull requests created before the cutoff are grandfathered."""

    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        (Path(self._tmp.name) / "repo").mkdir()
        self.repo = Repo(Path(self._tmp.name) / "repo")
        self.cutoff = Path(self._tmp.name) / "cutoff.txt"
        self.cutoff.write_text("# comment\n2026-10-09T00:00:00Z\n")
        self.repo.commit("feat: unsigned", HUMAN)

    def gate(self, created_at: str | None) -> subprocess.CompletedProcess[str]:
        extra = ["--cutoff-file", str(self.cutoff)]
        if created_at is not None:
            extra += ["--created-at", created_at]
        return self.repo.gate(*extra)

    def test_created_before_the_cutoff_is_grandfathered(self) -> None:
        res = self.gate("2026-10-08T23:59:59Z")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("grandfathered", res.stdout)

    def test_created_after_the_cutoff_is_enforced(self) -> None:
        self.assertEqual(self.gate("2026-10-09T00:00:01Z").returncode, 1)

    def test_created_exactly_at_the_cutoff_is_enforced(self) -> None:
        self.assertEqual(self.gate("2026-10-09T00:00:00Z").returncode, 1)

    def test_offset_timestamps_compare_in_utc(self) -> None:
        self.assertEqual(self.gate("2026-10-09T01:30:00+02:00").returncode, 0)
        self.assertEqual(self.gate("2026-10-09T02:30:00+02:00").returncode, 1)

    def test_no_creation_time_is_enforced(self) -> None:
        self.assertEqual(self.gate(None).returncode, 1)

    def test_signed_commit_after_the_cutoff_passes(self) -> None:
        self.repo.commit("feat: signed", HUMAN, signoff=True)
        self.repo.base = self.repo.git(["rev-parse", "HEAD~1"]).strip()
        self.assertEqual(self.gate("2026-10-10T00:00:00Z").returncode, 0)

    def test_unreadable_timestamp_or_cutoff_is_an_error(self) -> None:
        self.assertEqual(self.gate("yesterday").returncode, 2)
        self.cutoff.write_text("not a time\n")
        self.assertEqual(self.gate("2026-10-08T00:00:00Z").returncode, 2)
        self.cutoff.write_text("# only a comment\n")
        self.assertEqual(self.gate("2026-10-08T00:00:00Z").returncode, 2)
        self.cutoff.unlink()
        self.assertEqual(self.gate("2026-10-08T00:00:00Z").returncode, 2)


class DcoWiringTest(unittest.TestCase):
    """The repository wiring that makes the gate bind."""

    def read(self, rel: str) -> str:
        return (ROOT / rel).read_text(encoding="utf-8")

    def test_workflow_job_runs_the_script_with_the_pull_request_inputs(self) -> None:
        text = self.read(".github/workflows/rule-enforcement.yml")
        self.assertIn("name: DCO Sign-off", text)
        self.assertIn("python3 scripts/ci/check-dco.py", text)
        job = text.split("name: DCO Sign-off", 1)[1].split("  ffmpeg-patches-surface-check:", 1)[0]
        for var in (
            "PR_AUTHOR",
            "PR_AUTHOR_TYPE",
            "BASE_SHA",
            "HEAD_SHA",
            "DCO_RELEASE_PR",
            "PR_CREATED_AT",
        ):
            self.assertIn(var, job)

    def test_job_is_a_required_context(self) -> None:
        text = self.read(".github/workflows/required-aggregator.yml")
        self.assertIn("'DCO Sign-off'", text)

    def test_renovate_signs_off_its_commits(self) -> None:
        # Sign-off comes from ":gitSignOff" directly or through the shared
        # plurano/horistes preset, whose default.json extends it.
        cfg = json.loads(self.read("renovate.json"))
        signing = {":gitSignOff", "github>plurano/horistes"}
        self.assertTrue(signing & set(cfg["extends"]), cfg["extends"])

    def test_script_and_listed_bots_are_documented(self) -> None:
        doc = self.read("docs/development/dco.md")
        spec_lines = [
            ln
            for ln in self.read("scripts/ci/check-dco.py").splitlines()
            if ln.startswith("BOT_LOGINS")
        ]
        self.assertEqual(len(spec_lines), 1)
        for login in ("renovate[bot]", "dependabot[bot]", "github-actions[bot]"):
            self.assertIn(login, spec_lines[0])
            self.assertIn(login, doc)

    def test_cutoff_file_is_valid_and_stated_in_contributing(self) -> None:
        text = self.read("scripts/ci/dco-cutoff.txt")
        stamps = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("#")]
        self.assertEqual(len(stamps), 1)
        day = stamps[0][:10]
        self.assertRegex(stamps[0], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertIn(day, self.read("CONTRIBUTING.md"))

    def test_contributing_explains_the_sign_off(self) -> None:
        text = self.read("CONTRIBUTING.md")
        self.assertIn("## Developer Certificate of Origin", text)
        self.assertIn("git commit -s", text)


if __name__ == "__main__":
    unittest.main()
