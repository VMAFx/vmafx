#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Landed master-commit and workflow wiring coverage for ``scripts/ci/check-dco.py`` (ADR-3389)."""

from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.ci.tests import check_dco_fixture as fx  # noqa: E402
from scripts.lib.safe_subprocess import run  # noqa: E402

LANDED_COMMIT_CASES: tuple[tuple[str, str, tuple[str, str], int, str], ...] = (
    # PR #3042 squash commit with bot Co-authored-by trailer is refused.
    ("landed_3042_bot_coauthor", fx.LANDED_3042, fx.BOT_IDENT, 1, "Co-authored-by"),
    # Renovate commit lacking a Signed-off-by trailer is refused on master.
    ("renovate_missing_signoff", "chore(deps): Update X (#1)", fx.BOT_IDENT, 1, "Signed-off-by"),
    # Generated-attribution footer is refused on master even with valid sign-off.
    (
        "generated_footer",
        f"chore: add tool\n\nSigned-off-by: {fx.HUMAN[0]} <{fx.HUMAN[1]}>\n\nGenerated with Claude",
        fx.HUMAN,
        1,
        "machine-generated attribution",
    ),
    # Robot emoji in commit subject/body is refused on master.
    (
        "robot_emoji",
        f"chore: add tool 🤖\n\nSigned-off-by: {fx.HUMAN[0]} <{fx.HUMAN[1]}>",
        fx.HUMAN,
        1,
        "machine-generated attribution",
    ),
    # Human Co-authored-by trailer with matching author sign-off passes on master.
    (
        "human_coauthor_signed",
        (
            "feat: pair work\n\nSigned-off-by: Bob <bob@example.com>\n"
            f"Co-authored-by: {fx.HUMAN[0]} <{fx.HUMAN[1]}>"
        ),
        ("Bob", "bob@example.com"),
        0,
        "PASS",
    ),
    # Sign-off matching neither commit author nor landing identity fails on master.
    (
        "third_party_signoff",
        "chore(deps): update\n\nSigned-off-by: Third Party <charlie@example.org>",
        fx.BOT_IDENT,
        1,
        "matches neither",
    ),
    # Landing-identity sign-off authorizes a bot-authored commit on master.
    (
        "landing_identity_on_bot_author",
        f"chore(deps): update\n\nSigned-off-by: {fx.LANDING_ID}",
        fx.BOT_IDENT,
        0,
        "PASS",
    ),
)


class DcoLandedGateTest(unittest.TestCase):
    """Master-push landed commit checks (``--landed``) with exemptions disabled."""

    def setUp(self) -> None:
        """Create a disposable repository for landed-commit gate invocations."""
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = fx.Repo(Path(self._tmp.name))

    def test_landed_commit_message_and_signoff_cases(self) -> None:
        """Landed commits enforce sign-off authorization and refuse bot/tool attribution."""
        for name, msg, ident, expected_rc, expected_text in LANDED_COMMIT_CASES:
            with self.subTest(case=name), tempfile.TemporaryDirectory() as directory:
                repo = fx.Repo(Path(directory))
                repo.commit(msg, ident)
                res = repo.gate("--landed", "--landing-identity", fx.LANDING_ID)
                self.assertEqual(res.returncode, expected_rc, res.stdout + res.stderr)
                self.assertIn(expected_text, res.stdout)

    def test_model_author_identity_fails(self) -> None:
        """A landed coding tool author is refused with landing repair advice."""
        self.repo.commit("feat: model author", ("Claude", "claude@example.org"), True, fx.HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", fx.LANDING_ID)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("author identity", res.stdout)
        self.assertIn("landing tool", res.stdout)
        self.assertNotIn("re-author", res.stdout)

    def test_message_from_renovate_json_passes(self) -> None:
        """The configured Renovate commitBody template produces a valid landed message."""
        cfg = json.loads((fx.ROOT / "renovate.json").read_text(encoding="utf-8"))
        bot_str = f"{fx.BOT_IDENT[0]} <{fx.BOT_IDENT[1]}>"
        body = cfg["commitBody"].replace("{{{gitAuthor}}}", bot_str)
        msg = f"chore(deps): Update X (#1)\n\n{body}"
        self.repo.commit(msg, fx.BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", fx.LANDING_ID)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("PASS", res.stdout)

    def test_committer_only_match_fails(self) -> None:
        """Prove scripts/ci/check-dco.py --landed rejects committer-only matches.

        Requires sign-off to match the landing identity or author, rejecting
        committer-only matches.
        """
        n = len(list(self.repo.path.glob("f*.txt")))
        (self.repo.path / f"f{n}.txt").write_text(f"{n}\n", encoding="utf-8")
        self.repo.git(["add", "-A"], fx.BOT_IDENT)
        env = fx.git_env(fx.BOT_IDENT)
        env.update(
            GIT_COMMITTER_NAME="GitHub",
            GIT_COMMITTER_EMAIL="noreply@github.com",
        )
        msg = "chore(deps): update\n\nSigned-off-by: GitHub <noreply@github.com>"
        run(
            [fx.GIT, "-c", "commit.gpgsign=false", "commit", "-q", "-m", msg],
            allowed_executables=(fx.GIT,),
            cwd=self.repo.path,
            env=env,
            check=True,
            timeout_seconds=fx.GIT_TIMEOUT_S,
        )
        res = self.repo.gate("--landed", "--landing-identity", fx.LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("matches neither", res.stdout)

    def test_release_pr_with_landed_still_judges(self) -> None:
        """Landed mode ignores --release-pr and still requires a valid sign-off."""
        self.repo.commit("chore: release unsigned", fx.HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", fx.LANDING_ID, "--release-pr")
        self.assertEqual(res.returncode, 1)
        self.assertIn("Signed-off-by", res.stdout)

    def test_base_40_zeros_judges_head_only(self) -> None:
        """Prove scripts/ci/check-dco.py --landed evaluates only HEAD on 40-zero base."""
        cases = (
            ("feat: bad head unsigned", False, 1, "Signed-off-by"),
            ("feat: good head signed", True, 0, "PASS"),
        )
        for msg, signoff, expected_rc, expected_text in cases:
            with self.subTest(signoff=signoff):
                head = self.repo.commit(msg, fx.HUMAN, signoff=signoff)
                res = run(
                    [
                        sys.executable,
                        "-I",
                        str(fx.GATE),
                        "--landed",
                        "--landing-identity",
                        fx.LANDING_ID,
                        "--base",
                        "0" * 40,
                        "--head",
                        head,
                    ],
                    allowed_executables=(sys.executable,),
                    cwd=self.repo.path,
                    env=fx.clean_env(),
                    capture_output=True,
                    text=True,
                    check=False,
                    timeout_seconds=fx.SCRIPT_TIMEOUT_S,
                )
                self.assertEqual(res.returncode, expected_rc, res.stdout + res.stderr)
                self.assertIn(expected_text, res.stdout)
                self.assertIn("judging head commit only", res.stdout)

    def test_landed_without_identity_is_usage_error(self) -> None:
        """Landed mode without --landing-identity exits 2."""
        res = self.repo.gate("--landed")
        self.assertEqual(res.returncode, 2)
        self.assertIn("--landing-identity", res.stderr)


class DcoWiringTest(unittest.TestCase):
    """The repository wiring that makes the gate bind."""

    def read(self, rel: str) -> str:
        """Read a repository-relative UTF-8 text file."""
        return (fx.ROOT / rel).read_text(encoding="utf-8")

    def test_workflow_job_runs_the_script_with_the_pull_request_inputs(self) -> None:
        """The workflow supplies PR metadata and reads body text from the event file."""
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
        self.assertIn("--pr-body-from-event", job)
        self.assertNotIn("PR_BODY:", job)
        self.assertNotIn("${{ github.event.pull_request.body }}", job)

    def test_job_is_a_required_context(self) -> None:
        """Required-aggregator lists 'DCO Sign-off' so red runs block merges."""
        text = self.read(".github/workflows/required-aggregator.yml")
        self.assertIn("'DCO Sign-off'", text)

    def test_renovate_signs_off_its_commits(self) -> None:
        """Renovate configuration enables gitSignOff directly or via shared preset."""
        # Sign-off comes from ":gitSignOff" directly or through the shared
        # plurano/horistes preset, whose default.json extends it.
        cfg = json.loads(self.read("renovate.json"))
        signing = {":gitSignOff", "github>plurano/horistes"}
        self.assertTrue(signing & set(cfg["extends"]), cfg["extends"])

    def test_script_and_listed_bots_are_documented(self) -> None:
        """Every login in BOT_LOGINS is documented in docs/development/dco.md."""
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
        """The cutoff file holds one ISO-8601 UTC stamp whose date matches CONTRIBUTING.md."""
        text = self.read("scripts/ci/dco-cutoff.txt")
        stamps = [ln for ln in text.splitlines() if ln.strip() and not ln.startswith("#")]
        self.assertEqual(len(stamps), 1)
        day = stamps[0][:10]
        self.assertRegex(stamps[0], r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
        self.assertIn(day, self.read("CONTRIBUTING.md"))

    def test_contributing_explains_the_sign_off(self) -> None:
        """CONTRIBUTING.md documents the DCO section and git commit -s."""
        text = self.read("CONTRIBUTING.md")
        self.assertIn("## Developer Certificate of Origin", text)
        self.assertIn("git commit -s", text)

    def test_landed_commit_messages_workflow_and_renovate_wiring(self) -> None:
        """Master landed-commit workflow and Renovate commitBody stay wired."""
        wf_path = fx.ROOT / ".github" / "workflows" / "landed-commit-messages.yml"
        self.assertTrue(wf_path.exists(), "landed-commit-messages.yml missing")
        text = self.read(".github/workflows/landed-commit-messages.yml")
        self.assertIn("branches: [master]", text)
        self.assertIn("python3 scripts/ci/check-dco.py --landed", text)
        self.assertIn("BASE_SHA: ${{ github.event.before }}", text)
        self.assertIn("HEAD_SHA: ${{ github.sha }}", text)
        self.assertIn("LANDING_IDENTITY:", text)

        cfg = json.loads(self.read("renovate.json"))
        self.assertEqual(cfg.get("commitBody"), "Signed-off-by: {{{gitAuthor}}}")

    def test_bot_automerge_signoff_workflow_wiring(self) -> None:
        """Bot automerge sign-off workflow invokes fix-bot-automerge-body.py with required env."""
        wf_path = fx.ROOT / ".github" / "workflows" / "bot-automerge-signoff.yml"
        self.assertTrue(wf_path.exists(), "bot-automerge-signoff.yml missing")
        text = self.read(".github/workflows/bot-automerge-signoff.yml")
        self.assertIn("pull_request_target", text)
        self.assertIn("auto_merge_enabled", text)
        self.assertIn("python3 scripts/ci/fix-bot-automerge-body.py", text)
        for var in ("PR_NODE_ID", "PR_AUTHOR_TYPE", "AUTO_MERGE_BODY"):
            self.assertIn(var, text)


if __name__ == "__main__":
    unittest.main()
