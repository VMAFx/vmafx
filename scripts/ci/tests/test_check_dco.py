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
import multiprocessing
import os
import shlex
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from types import ModuleType

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT))

from scripts.lib.safe_subprocess import TextCommandResult, run  # noqa: E402

GATE = ROOT / "scripts" / "ci" / "check-dco.py"
GIT = shutil.which("git") or "/usr/bin/git"
GIT_TIMEOUT_S = 60
SCRIPT_TIMEOUT_S = 60
# Repository-path variables the gate subprocess must not inherit (see clean_env).
REPO_ENV = (
    "GIT_DIR",
    "GIT_WORK_TREE",
    "GIT_COMMON_DIR",
    "GIT_INDEX_FILE",
    "GIT_OBJECT_DIRECTORY",
    "GIT_ALTERNATE_OBJECT_DIRECTORIES",
    "GIT_NAMESPACE",
)
HUMAN = ("Ada Example", "ada@example.org")
BOT_LOGIN = "renovate[bot]"
BOT_IDENT = (BOT_LOGIN, "29139614+renovate[bot]@users.noreply.github.com")
LANDING_ID = "Lusoris <lusoris@proton.me>"

LANDED_3042 = """chore(deps): Update Python (patch) (#3042)

* chore(deps): Update Python (patch)

Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>

---------

Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>
Signed-off-by: Lusoris <lusoris@proton.me>
Co-authored-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>
Co-authored-by: Lusoris <lusoris@proton.me>
"""


def git_env(ident: tuple[str, str], committer: tuple[str, str] | None = None) -> dict[str, str]:
    """Prepare isolated fixture identities without inherited repository configuration."""
    committer = committer or ident
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    env.update(
        GIT_AUTHOR_NAME=ident[0],
        GIT_AUTHOR_EMAIL=ident[1],
        GIT_COMMITTER_NAME=committer[0],
        GIT_COMMITTER_EMAIL=committer[1],
        GIT_CONFIG_GLOBAL="/dev/null",
        GIT_CONFIG_SYSTEM="/dev/null",
    )
    return env


def clean_env() -> dict[str, str]:
    """The environment without the variables the gate reads, so a CI run cannot leak into a case."""
    drop = (
        *REPO_ENV,
        "BASE_SHA",
        "HEAD_SHA",
        "PR_AUTHOR",
        "PR_AUTHOR_TYPE",
        "PR_CREATED_AT",
        "DCO_RELEASE_PR",
        "LANDING_IDENTITY",
        "PR_BODY",
    )
    return {k: v for k, v in os.environ.items() if k not in drop}


class Repo:
    """A throwaway repository with an initial commit on ``main``."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self.git(["init", "-q", "-b", "main"])
        self.commit("initial", HUMAN, signoff=True)
        self.base = self.git(["rev-parse", "HEAD"]).strip()

    def git(
        self,
        args: list[str],
        ident: tuple[str, str] = HUMAN,
        committer: tuple[str, str] | None = None,
        stdin: str | None = None,
    ) -> str:
        """Run fixture Git commands for scripts/ci/check-dco.py.

        Sets up commits and git state in an isolated test repo.
        """
        done = run(
            [GIT, "-c", "commit.gpgsign=false", *args],
            allowed_executables=(GIT,),
            cwd=self.path,
            env=git_env(ident, committer),
            input_data=stdin,
            capture_output=True,
            text=True,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
        return done.stdout

    def commit(
        self,
        message: str,
        ident: tuple[str, str],
        signoff: bool = False,
        committer: tuple[str, str] | None = None,
    ) -> str:
        """Create a fixture commit with optional sign-off and return its revision."""
        n = len(list(self.path.glob("f*.txt")))
        (self.path / f"f{n}.txt").write_text(f"{n}\n")
        self.git(["add", "-A"], ident)
        flags = ["-s"] if signoff else []
        self.git(["commit", "-q", *flags, "-F", "-"], ident, committer, stdin=message)
        return self.git(["rev-parse", "HEAD"], ident).strip()

    def gate(
        self, *extra: str, pr_body: str | None = None, event: object | None = None
    ) -> TextCommandResult:
        """Run scripts/ci/check-dco.py against commits in the test repo.

        Runs the DCO gate against commits between base and head in the test repo.
        """
        head = self.git(["rev-parse", "HEAD"]).strip()
        env = clean_env()
        if pr_body is not None:
            env["PR_BODY"] = pr_body
        if event is not None:
            event_file = self.path / "event.json"
            event_file.write_text(json.dumps(event), encoding="utf-8")
            env["GITHUB_EVENT_PATH"] = str(event_file)
        return run(
            [sys.executable, "-I", str(GATE), "--base", self.base, "--head", head, *extra],
            allowed_executables=(sys.executable,),
            cwd=self.path,
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
        )


def load_gate(name: str) -> ModuleType:
    """Load the policy module under a registered name for dataclass resolution."""
    spec = importlib.util.spec_from_file_location(name, GATE)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = mod
    spec.loader.exec_module(mod)
    return mod


def assert_no_machine_attribution(line: str) -> None:
    """Check a potentially expensive line inside a disposable child process."""
    mod = load_gate("check_dco_child")
    assert mod.check_machine_attribution(line) is False


ADVERSARIAL_PREFIXES = (
    ("hashes", "#" * 64 + "x"),
    ("whitespace", " " * 5000 + "x"),
    ("bullets", "- " * 5000 + "x"),
    ("comments", "<!-- " * 2000 + "x"),
)


def run_prefix_scan_child(line: str) -> str | None:
    """Scan one line in a spawned child; return the failure, or None when it passes in time."""
    child = multiprocessing.get_context("spawn").Process(
        target=assert_no_machine_attribution, args=(line,)
    )
    child.start()
    try:
        child.join(timeout=2)
        if child.is_alive():
            return "attribution scan exceeded two seconds"
        if child.exitcode != 0:
            return "non-attribution line was refused or child failed"
        return None
    finally:
        if child.is_alive():
            child.kill()
            child.join(timeout=2)
        child.close()


class DcoPrefixScanTimeTest(unittest.TestCase):
    def test_machine_attribution_prefix_scan_finishes(self) -> None:
        """Adversarial prefixes return False within two seconds, even on a broken scanner."""
        for name, line in ADVERSARIAL_PREFIXES:
            with self.subTest(prefix=name):
                self.assertIsNone(run_prefix_scan_child(line))


class DcoBoundsTest(unittest.TestCase):
    def setUp(self) -> None:
        """Load a fresh policy module for boundary cases and register module cleanup."""
        self.mod = load_gate("check_dco_bounds")
        self.addCleanup(sys.modules.pop, "check_dco_bounds", None)

    def test_machine_attribution_prefixes_and_non_attribution(self) -> None:
        """Prefix removal preserves generated phrases, word boundaries, and robot emoji."""
        for line, expected in (
            ("<!-- Generated with X", True),
            ("> - Created by X", True),
            ("### generated by", True),
            ("generator by", False),
            ("regenerated with", False),
            ("Generated byproduct", False),
            ("", False),
            ("# > - * <!--\t", False),
            ("Human text 🤖", True),
        ):
            with self.subTest(line=line):
                self.assertIs(self.mod.check_machine_attribution(line), expected)

    def test_commit_message_bytes_at_bound_and_plus_one(self) -> None:
        """A multibyte commit message passes at the byte bound and fails one byte over."""
        bound = self.mod.MAX_COMMIT_MESSAGE_BYTES
        for extra in (0, 1):
            with self.subTest(extra=extra):
                text = "é" * (bound // 2) + "a" * (bound % 2 + extra)
                commit = self.mod.Commit("a" * 40, *HUMAN, *HUMAN, "subject", text)
                reason = self.mod.commit_attribution_reason(commit)
                if extra:
                    self.assertIn("MAX_COMMIT_MESSAGE_BYTES", reason or "")
                else:
                    self.assertIsNone(reason)

    def test_commit_message_lines_at_bound_and_plus_one(self) -> None:
        """A commit message passes at the line bound and fails one line over."""
        for extra in (0, 1):
            with self.subTest(extra=extra):
                text = "x\n" * (self.mod.MAX_COMMIT_MESSAGE_LINES + extra)
                commit = self.mod.Commit("a" * 40, *HUMAN, *HUMAN, "subject", text)
                reason = self.mod.commit_attribution_reason(commit)
                if extra:
                    self.assertIn("MAX_COMMIT_MESSAGE_LINES", reason or "")
                else:
                    self.assertIsNone(reason)

    def test_pr_body_bytes_at_bound_and_plus_one(self) -> None:
        """A multibyte PR body passes at the byte bound and fails one byte over."""
        bound = self.mod.MAX_PR_BODY_BYTES
        for extra in (0, 1):
            with self.subTest(extra=extra):
                text = "é" * (bound // 2) + "a" * (bound % 2 + extra)
                findings = self.mod.pr_body_attribution(text)
                if extra:
                    self.assertIn("MAX_PR_BODY_BYTES", findings[0][1])
                else:
                    self.assertEqual(findings, [])

    def test_pr_body_lines_at_bound_and_plus_one(self) -> None:
        """A PR body passes at the line bound and fails one line over."""
        for extra in (0, 1):
            with self.subTest(extra=extra):
                text = "x\n" * (self.mod.MAX_PR_BODY_LINES + extra)
                findings = self.mod.pr_body_attribution(text)
                if extra:
                    self.assertIn("MAX_PR_BODY_LINES", findings[0][1])
                else:
                    self.assertEqual(findings, [])

    def test_nul_records_preserve_old_separators_and_reject_malformed(self) -> None:
        """NUL-delimited records preserve old separators; malformed field counts are refused."""
        fields = ["a" * 40, "Ada\x1fExample", HUMAN[1], *HUMAN, "s", "s\x1etext"]
        valid = "\0".join(fields) + "\0"
        parsed = self.mod.parse_commits(valid)
        self.assertEqual(len(parsed), 1)
        self.assertEqual(parsed[0].author_name, fields[1])
        for malformed in (valid[:-1], valid + "extra\0", "\0"):
            with self.subTest(record=repr(malformed)):
                with self.assertRaises(self.mod.UsageError):
                    self.mod.parse_commits(malformed)


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

    def test_old_separator_identity_cannot_hide_tool(self) -> None:
        """Old separators cannot conceal a coding tool in the author identity."""
        ident = ("Ada\x1fada@example.org\x1fOther\x1fother@example.org\x1fClaude", HUMAN[1])
        self.repo.commit("feat: delimiter", ident, True, HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("author identity", res.stdout)
        self.assertIn("claude", res.stdout)

    def test_old_separators_in_human_identity_and_message_pass(self) -> None:
        """Old separators in human identities and message text remain acceptable."""
        self.repo.commit(
            "feat: delimiter\n\nHuman\x1ftext\x1emore", ("Ada\x1fOther", HUMAN[1]), True
        )
        res = self.repo.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_unicode_prefixed_attribution_refused_in_messages_and_bodies(self) -> None:
        """Invisible Unicode prefixes cannot hide attribution in commit messages or PR bodies."""
        self.repo.commit("feat: signed", HUMAN, signoff=True)
        for prefix in ("\ufeff", "\u200b", "\u200c", "\u200d", "\u2060", " \ufeff "):
            for line in ("Generated by assistant", "Co-authored-by: Codex <pair@example.org>"):
                with self.subTest(prefix=repr(prefix), line=line):
                    res = self.repo.gate(pr_body=prefix + line)
                    self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.repo.commit("feat: hidden\n\n\ufeffGenerated by assistant", HUMAN, True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("message contains machine-generated attribution", res.stdout)

    def test_nfkc_attribution_refused_and_human_text_passes(self) -> None:
        """Compatibility characters cannot hide attribution; human text still passes."""
        for line in (
            "\uff27\uff45\uff4e\uff45\uff52\uff41\uff54\uff45\uff44 \uff42\uff59 assistant",
            "\uff23\uff4f-\uff41\uff55\uff54\uff48\uff4f\uff52\uff45\uff44-\uff42\uff59: \uff23\uff4f\uff44\uff45\uff58 <pair@example.org>",
        ):
            with self.subTest(line=line):
                res = self.repo.gate(pr_body=line)
                self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        res = self.repo.gate(
            pr_body="\ufeffHuman contribution\nCo-authored-by: Alice <a@example.org>"
        )
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_event_body_refuses_attribution(self) -> None:
        """Attribution in an event body is refused with the offending line number."""
        event = {"pull_request": {"body": "Summary\n\ufeffGenerated by assistant"}}
        res = self.repo.gate("--pr-body-from-event", event=event)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("line 2:", res.stdout)

    def test_event_clean_empty_and_null_bodies_pass(self) -> None:
        """Clean, empty, and null event bodies all pass attribution checks."""
        for body in ("Human contribution", "", None):
            with self.subTest(body=body):
                res = self.repo.gate("--pr-body-from-event", event={"pull_request": {"body": body}})
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_squash_workflow_body_file_preserves_event_text(self) -> None:
        """Squash body extraction preserves multiline and large multibyte event text."""
        workflow = (ROOT / ".github/workflows/rule-enforcement.yml").read_text()
        extraction = next(
            line.strip()
            for line in workflow.splitlines()
            if "jq -r" in line and "squash-pr-body" in line
        )
        self.assertIn('--body-file "$RUNNER_TEMP/squash-pr-body.md"', workflow)
        event = self.repo.path / "event.json"
        body = "Summary\nMigration: preserve boundary\n" + "é" * 70_000
        event.write_text(json.dumps({"pull_request": {"body": body}}), encoding="utf-8")
        command = shlex.split(extraction)
        result = run(
            [*command[:3], str(event)],
            allowed_executables=("jq",),
            capture_output=True,
            text=True,
            check=True,
        )
        self.assertEqual(result.stdout, body + "\n")

    def test_event_invalid_shape_refused(self) -> None:
        """Missing fields, non-object events, and non-text bodies produce usage errors."""
        events: tuple[object, ...] = ({}, [], {"pull_request": {"body": 42}})
        for event in events:
            with self.subTest(event=event):
                res = self.repo.gate("--pr-body-from-event", event=event)
                self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
                self.assertIn("event", res.stderr)

    def test_event_envelope_bytes_at_bound_and_plus_one(self) -> None:
        """An event envelope passes at the byte bound and fails one byte over."""
        mod = DcoBoundsTest()
        mod.setUp()
        self.addCleanup(mod.doCleanups)
        event = {"pull_request": {"body": ""}, "padding": ""}
        size = len(json.dumps(event).encode("utf-8"))
        for extra in (0, 1):
            with self.subTest(extra=extra):
                event["padding"] = "a" * (mod.mod.MAX_PR_EVENT_BYTES - size + extra)
                res = self.repo.gate("--pr-body-from-event", event=event)
                self.assertEqual(res.returncode, extra, res.stdout + res.stderr)
                if extra:
                    self.assertIn("MAX_PR_EVENT_BYTES", res.stderr)

    def test_pr_body_file_bytes_at_bound_and_plus_one(self) -> None:
        """A body file passes at the byte bound and fails one byte over."""
        mod = DcoBoundsTest()
        mod.setUp()
        self.addCleanup(mod.doCleanups)
        body = self.repo.path / "body.md"
        for extra in (0, 1):
            with self.subTest(extra=extra):
                body.write_bytes(b"a" * (mod.mod.MAX_PR_BODY_BYTES + extra))
                res = self.repo.gate("--pr-body", str(body))
                self.assertEqual(res.returncode, extra, res.stdout + res.stderr)
                if extra:
                    self.assertIn("MAX_PR_BODY_BYTES", res.stderr)

    def test_event_body_byte_bound_and_large_multibyte_body(self) -> None:
        """A multibyte event body passes at the byte bound and fails one byte over."""
        mod = DcoBoundsTest()
        mod.setUp()
        self.addCleanup(mod.doCleanups)
        bound = mod.mod.MAX_PR_BODY_BYTES
        for extra in (0, 1):
            with self.subTest(extra=extra):
                body = "é" * (bound // 2) + "a" * (bound % 2 + extra)
                res = self.repo.gate("--pr-body-from-event", event={"pull_request": {"body": body}})
                self.assertEqual(res.returncode, extra, res.stdout + res.stderr)
                if extra:
                    self.assertIn("MAX_PR_BODY_BYTES", res.stdout)

    def test_commit_message_byte_bound_through_git(self) -> None:
        """A stored commit message passes at the byte bound and fails one byte over."""
        mod = DcoBoundsTest()
        mod.setUp()
        self.addCleanup(mod.doCleanups)
        prefix = "feat: bounded\n\n"
        suffix = "\n\nSigned-off-by: Ada Example <ada@example.org>\n"
        for extra in (0, 1):
            with self.subTest(extra=extra):
                size = mod.mod.MAX_COMMIT_MESSAGE_BYTES + extra
                message = prefix + "a" * (size - len(prefix + suffix)) + suffix
                self.repo.commit(message, HUMAN)
                res = self.repo.gate()
                self.assertEqual(res.returncode, extra, res.stdout + res.stderr)
                if extra:
                    self.assertIn("MAX_COMMIT_MESSAGE_BYTES", res.stdout)

    def test_oversized_git_output_refused_before_parse(self) -> None:
        """An oversized commit log is refused with log and message bounds identified."""
        mod = DcoBoundsTest()
        mod.setUp()
        self.addCleanup(mod.doCleanups)
        self.repo.commit("feat: enormous\n\n" + "x" * mod.mod.MAX_COMMIT_LOG_BYTES, HUMAN, True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("MAX_COMMIT_LOG_BYTES", res.stderr)
        self.assertIn("MAX_COMMIT_MESSAGE_BYTES", res.stderr)

    def test_planted_full_attribution_gap_fails(self) -> None:
        """Combined coding tool attribution is refused and the offending commit is identified."""
        msg = "feat: planted gap\n\nCo-Authored-By: Claude <noreply@anthropic.com>"
        sha = self.repo.commit(msg, ("Claude", "noreply@anthropic.com"), True, HUMAN)
        res = self.repo.gate(pr_body="Generated by Claude Code")
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn(sha[:10], res.stdout)
        self.assertIn("claude", res.stdout.lower())

    def test_author_identity_alone_fails(self) -> None:
        """A coding tool author is refused with re-authoring advice instead of sign-off advice."""
        self.repo.commit("feat: model author", ("Claude", "claude@example.org"), True, HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("author identity", res.stdout)
        self.assertIn("re-author affected commits", res.stdout)
        self.assertNotIn("git rebase --signoff", res.stdout)

    def test_author_email_alone_fails(self) -> None:
        """A coding tool email is refused even when the author name is human."""
        self.repo.commit("feat: model email", ("Ada Example", "noreply@anthropic.com"), True, HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("author identity", res.stdout)
        self.assertIn("anthropic", res.stdout)

    def test_committer_identity_alone_fails(self) -> None:
        """A coding tool committer is refused despite a human author and valid sign-off."""
        self.repo.commit("feat: model committer", HUMAN, True, ("Codex", "codex@example.org"))
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("committer identity", res.stdout)

    def test_machine_coauthor_trailer_alone_fails(self) -> None:
        """A coding tool coauthor trailer is refused despite a human author and sign-off."""
        msg = "feat: pair work\n\nCo-authored-by: Claude <noreply@anthropic.com>"
        self.repo.commit(msg, HUMAN, signoff=True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("Co-authored-by", res.stdout)

    def test_generated_footer_alone_fails(self) -> None:
        """A generated-attribution footer is refused despite a valid human sign-off."""
        self.repo.commit("feat: generated\n\n🤖 Generated with Claude Code", HUMAN, signoff=True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("machine-generated attribution", res.stdout)

    def test_human_coauthor_passes(self) -> None:
        """A human coauthor trailer passes when the commit has a valid sign-off."""
        msg = "feat: pair work\n\nCo-authored-by: Alice <alice@example.com>"
        self.repo.commit(msg, HUMAN, signoff=True)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_generated_pr_body_file_fails_with_line_number(self) -> None:
        """A generated-attribution body file is refused with the offending line number."""
        self.repo.commit("feat: signed", HUMAN, signoff=True)
        body = self.repo.path / "body.md"
        body.write_text("Summary\nGenerated by Claude Code\n")
        res = self.repo.gate("--pr-body-file", str(body))
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("::error title=PR body attribution::line 2:", res.stdout)

    def test_machine_coauthor_pr_body_file_fails(self) -> None:
        """A coding tool coauthor in a body file is refused as attribution."""
        body = self.repo.path / "body.md"
        body.write_text("Co-authored-by: Claude <noreply@anthropic.com>\n")
        res = self.repo.gate("--pr-body-file", str(body))
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("Co-authored-by", res.stdout)

    def test_clean_and_empty_pr_body_files_pass(self) -> None:
        """Both human-authored and empty body files pass attribution checks."""
        body = self.repo.path / "body.md"
        for content in ("Summary\nHuman contribution\n", ""):
            with self.subTest(content=content):
                body.write_text(content)
                res = self.repo.gate("--pr-body-file", str(body))
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_pr_body_environment_matches_file(self) -> None:
        """The same forbidden body attribution fails through file and environment inputs."""
        body = "Generated by Claude Code"
        body_file = self.repo.path / "body.md"
        body_file.write_text(body)
        from_file = self.repo.gate("--pr-body-file", str(body_file))
        from_env = self.repo.gate(pr_body=body)
        self.assertEqual(from_file.returncode, 1)
        self.assertEqual(from_env.returncode, from_file.returncode)
        self.assertIn("PR body attribution", from_env.stdout)

    def test_pr_body_file_takes_precedence_over_environment(self) -> None:
        """A clean explicit body file overrides forbidden attribution in the environment."""
        body_file = self.repo.path / "body.md"
        body_file.write_text("Human contribution\n")
        res = self.repo.gate("--pr-body-file", str(body_file), pr_body="Generated by Claude Code")
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)

    def test_missing_pr_body_file_is_usage_error(self) -> None:
        """A missing body file produces a usage error naming the body-file input."""
        res = self.repo.gate("--pr-body-file", str(self.repo.path / "missing.md"))
        self.assertEqual(res.returncode, 2, res.stdout + res.stderr)
        self.assertIn("PR body file", res.stderr)

    def test_planted_unsigned_commit_fails(self) -> None:
        """A commit without sign-off is refused with its subject and sign-off repair advice."""
        self.repo.commit("feat: unsigned", HUMAN)
        res = self.repo.gate()
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("::error title=DCO sign-off::", res.stdout)
        self.assertIn("feat: unsigned", res.stdout)
        self.assertIn("git rebase --signoff", res.stdout)

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
        """Prove scripts/ci/check-dco.py rejects an unresolvable revision range.

        Verifies that an unresolvable base/head revision range exits 2 instead of passing.
        """
        res = run(
            [sys.executable, "-I", str(GATE), "--base", "0" * 40, "--head", "HEAD"],
            allowed_executables=(sys.executable,),
            cwd=self.repo.path,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
        )
        self.assertEqual(res.returncode, 2)

    def test_missing_arguments_is_usage_error(self) -> None:
        """Prove scripts/ci/check-dco.py exits 2 when revision arguments are missing.

        Omitting both CLI revision flags and environment fallbacks must exit 2.
        """
        env = {k: v for k, v in os.environ.items() if k not in ("BASE_SHA", "HEAD_SHA")}
        res = run(
            [sys.executable, "-I", str(GATE)],
            allowed_executables=(sys.executable,),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
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

    def test_attribution_is_checked_on_exempt_bot_commit(self) -> None:
        """A bot sign-off exemption does not permit a coding tool coauthor trailer."""
        msg = "chore(deps): bump\n\nCo-authored-by: Claude <noreply@anthropic.com>"
        self.repo.commit(msg, BOT_IDENT)
        res = self.repo.gate("--author", BOT_LOGIN, "--author-type", "Bot")
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("Co-authored-by", res.stdout)

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
        """Every listed bot receives the exemption for commits matching its own identity."""
        spec = importlib.util.spec_from_file_location("check_dco", GATE)
        assert spec is not None and spec.loader is not None
        mod = importlib.util.module_from_spec(spec)
        sys.modules["check_dco"] = mod
        self.addCleanup(sys.modules.pop, "check_dco", None)
        spec.loader.exec_module(mod)
        for login in mod.BOT_LOGINS:
            commit = mod.Commit(
                "a" * 40, login, f"1+{login}@users.noreply.github.com", "Other", "x@y", "s", "s"
            )
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

    def gate(self, created_at: str | None) -> TextCommandResult:
        """Evaluate the unsigned fixture against a supplied creation time and local cutoff."""
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


class DcoLandedGateTest(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = Repo(Path(self._tmp.name))

    def test_landed_3042_by_bot_author_refused_for_coauthor(self) -> None:
        self.repo.commit(LANDED_3042, BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Co-authored-by", res.stdout)

    def test_model_author_identity_fails(self) -> None:
        """A landed coding tool author is refused with landing repair advice."""
        self.repo.commit("feat: model author", ("Claude", "claude@example.org"), True, HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1, res.stdout + res.stderr)
        self.assertIn("author identity", res.stdout)
        self.assertIn("landing tool", res.stdout)
        self.assertNotIn("re-author", res.stdout)

    def test_message_from_renovate_json_passes(self) -> None:
        cfg = json.loads((ROOT / "renovate.json").read_text(encoding="utf-8"))
        bot_str = f"{BOT_IDENT[0]} <{BOT_IDENT[1]}>"
        body = cfg["commitBody"].replace("{{{gitAuthor}}}", bot_str)
        msg = f"chore(deps): Update X (#1)\n\n{body}"
        self.repo.commit(msg, BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("PASS", res.stdout)

    def test_renovate_commit_body_missing_signoff_fails(self) -> None:
        msg = "chore(deps): Update X (#1)"
        self.repo.commit(msg, BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Signed-off-by", res.stdout)

    def test_generated_footer_fails(self) -> None:
        msg = f"chore: add tool\n\nSigned-off-by: {HUMAN[0]} <{HUMAN[1]}>\n\nGenerated with Claude"
        self.repo.commit(msg, HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("machine-generated attribution", res.stdout)

    def test_robot_emoji_fails(self) -> None:
        msg = f"chore: add tool 🤖\n\nSigned-off-by: {HUMAN[0]} <{HUMAN[1]}>"
        self.repo.commit(msg, HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("machine-generated attribution", res.stdout)

    def test_human_coauthor_with_signoff_passes(self) -> None:
        msg = f"feat: pair work\n\nSigned-off-by: Bob <bob@example.com>\nCo-authored-by: {HUMAN[0]} <{HUMAN[1]}>"
        self.repo.commit(msg, ("Bob", "bob@example.com"))
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("PASS", res.stdout)

    def test_signoff_only_by_third_party_fails(self) -> None:
        msg = "chore(deps): update\n\nSigned-off-by: Third Party <charlie@example.org>"
        self.repo.commit(msg, BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("matches neither", res.stdout)

    def test_landing_identity_signoff_on_bot_author_passes(self) -> None:
        msg = f"chore(deps): update\n\nSigned-off-by: {LANDING_ID}"
        self.repo.commit(msg, BOT_IDENT)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("PASS", res.stdout)

    def test_committer_only_match_fails(self) -> None:
        """Prove scripts/ci/check-dco.py --landed rejects committer-only matches.

        Requires sign-off to match the landing identity or author, rejecting
        committer-only matches.
        """
        n = len(list(self.repo.path.glob("f*.txt")))
        (self.repo.path / f"f{n}.txt").write_text(f"{n}\n")
        self.repo.git(["add", "-A"], BOT_IDENT)
        env = git_env(BOT_IDENT)
        env.update(
            GIT_COMMITTER_NAME="GitHub",
            GIT_COMMITTER_EMAIL="noreply@github.com",
        )
        msg = "chore(deps): update\n\nSigned-off-by: GitHub <noreply@github.com>"
        run(
            [GIT, "-c", "commit.gpgsign=false", "commit", "-q", "-m", msg],
            allowed_executables=(GIT,),
            cwd=self.repo.path,
            env=env,
            check=True,
            timeout_seconds=GIT_TIMEOUT_S,
        )
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID)
        self.assertEqual(res.returncode, 1)
        self.assertIn("matches neither", res.stdout)

    def test_release_pr_with_landed_still_judges(self) -> None:
        self.repo.commit("chore: release unsigned", HUMAN)
        res = self.repo.gate("--landed", "--landing-identity", LANDING_ID, "--release-pr")
        self.assertEqual(res.returncode, 1)
        self.assertIn("Signed-off-by", res.stdout)

    def test_base_40_zeros_judges_head_only(self) -> None:
        """Prove scripts/ci/check-dco.py --landed evaluates only HEAD on 40-zero base.

        Verifies that a 40-zero base SHA evaluates strictly the single HEAD commit.
        """
        # Bad head: unsigned commit -> rc 1
        self.repo.commit("feat: bad head unsigned", HUMAN)
        head_bad = self.repo.git(["rev-parse", "HEAD"]).strip()
        res_bad = run(
            [
                sys.executable,
                "-I",
                str(GATE),
                "--landed",
                "--landing-identity",
                LANDING_ID,
                "--base",
                "0" * 40,
                "--head",
                head_bad,
            ],
            allowed_executables=(sys.executable,),
            cwd=self.repo.path,
            env=clean_env(),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
        )
        self.assertEqual(res_bad.returncode, 1)
        self.assertIn("Signed-off-by", res_bad.stdout)
        self.assertIn("judging head commit only", res_bad.stdout)

        # Good head: signed commit -> rc 0
        self.repo.commit("feat: good head signed", HUMAN, signoff=True)
        head_good = self.repo.git(["rev-parse", "HEAD"]).strip()
        res_good = run(
            [
                sys.executable,
                "-I",
                str(GATE),
                "--landed",
                "--landing-identity",
                LANDING_ID,
                "--base",
                "0" * 40,
                "--head",
                head_good,
            ],
            allowed_executables=(sys.executable,),
            cwd=self.repo.path,
            env=clean_env(),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=SCRIPT_TIMEOUT_S,
        )
        self.assertEqual(res_good.returncode, 0, res_good.stdout + res_good.stderr)
        self.assertIn("PASS", res_good.stdout)
        self.assertIn("judging head commit only", res_good.stdout)

    def test_landed_without_identity_is_usage_error(self) -> None:
        res = self.repo.gate("--landed")
        self.assertEqual(res.returncode, 2)
        self.assertIn("--landing-identity", res.stderr)


class DcoWiringTest(unittest.TestCase):
    """The repository wiring that makes the gate bind."""

    def read(self, rel: str) -> str:
        return (ROOT / rel).read_text(encoding="utf-8")

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

    def test_landed_commit_messages_workflow_and_renovate_wiring(self) -> None:
        wf_path = ROOT / ".github" / "workflows" / "landed-commit-messages.yml"
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
        wf_path = ROOT / ".github" / "workflows" / "bot-automerge-signoff.yml"
        self.assertTrue(wf_path.exists(), "bot-automerge-signoff.yml missing")
        text = self.read(".github/workflows/bot-automerge-signoff.yml")
        self.assertIn("pull_request_target", text)
        self.assertIn("auto_merge_enabled", text)
        self.assertIn("python3 scripts/ci/fix-bot-automerge-body.py", text)
        for var in ("PR_NODE_ID", "PR_AUTHOR_TYPE", "AUTO_MERGE_BODY"):
            self.assertIn(var, text)


if __name__ == "__main__":
    unittest.main()
