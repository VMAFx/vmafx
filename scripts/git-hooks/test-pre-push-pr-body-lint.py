#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the PR-body hook's metadata lookup and release-PR exemption.

The lookup must stay bounded and fail closed. The release-PR exemption
(ADR-1151) must accept exactly what CI's Deliverables Checklist accepts: a
``release-please--`` head ref authored by a bot, as judged by the shared
``scripts/ci/release-pr-exempt.sh`` predicate.
"""

from __future__ import annotations

import asyncio
import html
import json
import os
import shutil
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

SCRIPT = Path(__file__).with_name("pre-push-pr-body-lint.sh").resolve()
EXEMPTION = SCRIPT.parents[2] / "scripts/ci/release-pr-exempt.sh"
GIT = Path(shutil.which("git") or "/usr/bin/git")

RELEASE_REF = "release-please--branches--master--components--vmafx"
# The stub body release-please writes when the changelog exceeds GitHub's
# body limit (#1213). It carries none of the ADR-0108 deliverables.
RELEASE_BODY = (
    "This release is too large to preview in the pull request body. View the "
    "full release notes here: https://github.com/VMAFx/vmafx/blob/"
    f"{RELEASE_REF}--release-notes/release-notes.md"
)
# `gh pr view --json author` marshals every non-User actor as
# {"is_bot": true, "login": "app/<login>"} and a User as
# {"is_bot": false, "login", "id", "name"} (cli/cli api/queries_issue.go).
GITHUB_ACTIONS = {"is_bot": True, "login": "app/github-actions"}
HUMAN = {"is_bot": False, "login": "lusoris", "id": "U_kgDOAAAAAQ", "name": "Lusoris"}
VALIDATOR_MARKER = "validator-ran"
ABSENT = object()


@dataclass(frozen=True)
class CommandResult:
    returncode: int
    stdout: str
    stderr: str


async def execute(
    command: list[str],
    *,
    cwd: Path,
    environment: dict[str, str],
    timeout: float = 10,
) -> CommandResult:
    process = await asyncio.create_subprocess_exec(
        *command,
        cwd=cwd,
        env=environment,
        stdout=asyncio.subprocess.PIPE,
        stderr=asyncio.subprocess.PIPE,
    )
    try:
        stdout, stderr = await asyncio.wait_for(process.communicate(), timeout)
    except TimeoutError:
        process.kill()
        await process.communicate()
        raise
    return CommandResult(process.returncode or 0, stdout.decode(), stderr.decode())


class HookFixture(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.bin = self.root / "bin"
        self.bin.mkdir()
        (self.root / "scripts/ci").mkdir(parents=True)
        validator = self.root / "scripts/ci/validate-pr-body.sh"
        validator.write_text("#!/usr/bin/env bash\nexit 0\n")
        validator.chmod(0o700)
        self.environment = {
            key: value
            for key, value in os.environ.items()
            if not key.startswith(("GIT_", "GH_", "PRE_COMMIT_"))
        }
        self.environment.update(
            GIT_CONFIG_NOSYSTEM="1",
            GIT_CONFIG_GLOBAL=os.devnull,
            PATH=str(self.bin) + os.pathsep + self.environment["PATH"],
            VMAFX_PR_LOOKUP_TIMEOUT_SECONDS="1",
        )
        self.git("init", "-q", "--initial-branch=master")
        self.git("config", "user.name", "Fixture")
        self.git("config", "user.email", "fixture@example.invalid")
        self.git("remote", "add", "origin", "git@github.com:VMAFx/vmafx.git")
        self.git("commit", "--allow-empty", "-qm", "base")
        self.git("update-ref", "refs/remotes/origin/master", "HEAD")
        self.git("switch", "-qc", "fix/pr-lookup")

    def git(self, *arguments: str) -> None:
        result = asyncio.run(
            execute(
                [str(GIT), *arguments],
                cwd=self.root,
                environment=self.environment,
            )
        )
        self.assertEqual(
            result.returncode,
            0,
            f"git {' '.join(arguments)} failed:\n{result.stderr}",
        )

    def write_tool(self, name: str, body: str) -> None:
        tool = self.bin / name
        tool.write_text("#!/usr/bin/env bash\nset -euo pipefail\n" + body)
        tool.chmod(0o700)

    def write_public_page_stub(
        self, *, draft: bool = True, body: str = "## Body\n\n- [x] ok"
    ) -> None:
        status = '<span data-status="draft"></span>\n' if draft else ""
        value = html.escape(body, quote=True).replace("\n", "&#10;")
        self.write_tool(
            "curl",
            'case "${*: -1}" in\n'
            "  */pulls) printf '%s\\n' '<a href=\"/VMAFx/vmafx/pull/1514\">PR</a>' ;;\n"
            "  */pull/1514) cat <<'EOF'\n"
            '<div class="js-pull-header-details" data-pull-is-open="true"></div>\n'
            f"{status}"
            f'<clipboard-copy role="menuitem" value="{value}"></clipboard-copy>\n'
            "EOF\n"
            "  ;;\n"
            "  *) exit 22 ;;\n"
            "esac\n",
        )

    def run_hook(self) -> CommandResult:
        return asyncio.run(
            execute(
                [str(SCRIPT)],
                cwd=self.root,
                environment=self.environment,
                timeout=4,
            )
        )


class PullRequestLookup(HookFixture):
    def test_hung_gh_falls_back_to_public_page_for_draft(self) -> None:
        self.write_tool("gh", "exec sleep 30\n")
        self.write_public_page_stub()

        result = self.run_hook()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("is a draft", result.stderr)

    def test_both_metadata_lookups_failing_blocks_push(self) -> None:
        self.write_tool("gh", "exit 1\n")
        self.write_tool("curl", "exit 22\n")

        result = self.run_hook()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("cannot determine PR state", result.stderr)


class ReleasePullRequestExemption(HookFixture):
    """The hook skips validation only where CI's deliverables gate does."""

    def setUp(self) -> None:
        super().setUp()
        predicate = self.root / "scripts/ci/release-pr-exempt.sh"
        shutil.copy2(EXEMPTION, predicate)
        # Stand in for the six `ADR-0108 missing deliverable` errors the real
        # validator prints for the release-please stub body.
        validator = self.root / "scripts/ci/validate-pr-body.sh"
        validator.write_text(f"#!/usr/bin/env bash\necho {VALIDATOR_MARKER} >&2\nexit 1\n")
        self.pull = self.root / "pull.json"

    def open_pull(self, branch: str, **metadata: object) -> None:
        """Check out *branch* and serve its open PR from a `gh` stub."""
        self.git("switch", "-qc", branch)
        self.serve_pull(branch, **metadata)

    def serve_pull(self, branch: str, **metadata: object) -> None:
        """Serve *branch*'s open PR, overriding the bot release PR's fields.

        The stub returns only the fields the hook asks for, as `gh pr view
        --json` does, so a hook that stops requesting a field sees it vanish.
        A field set to ``ABSENT`` is left out of the metadata altogether.
        """
        pull = {
            "author": GITHUB_ACTIONS,
            "body": RELEASE_BODY,
            "headRefName": branch,
            "isDraft": False,
            "state": "OPEN",
        }
        pull.update(metadata)
        pull = {key: value for key, value in pull.items() if value is not ABSENT}
        self.pull.write_text(json.dumps(pull))
        self.write_tool(
            "gh",
            f'[ "$1 $2 $3 $4" = "pr view {branch} --json" ] || exit 64\n'
            f'exec python3 - "$5" "{self.pull}" <<\'PY\'\n'
            "import json\n"
            "import sys\n"
            "with open(sys.argv[2], encoding='utf-8') as handle:\n"
            "    pull = json.load(handle)\n"
            "fields = sys.argv[1].split(',')\n"
            "json.dump({key: pull[key] for key in fields if key in pull}, sys.stdout)\n"
            "PY\n",
        )

    def assert_exempt(self, result: CommandResult) -> None:
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(VALIDATOR_MARKER, result.stderr)
        self.assertIn("exempt=true", result.stderr)

    def assert_validated(self, result: CommandResult, reason: str) -> None:
        """The validator ran and blocked the push, for the stated *reason*."""
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(VALIDATOR_MARKER, result.stderr)
        self.assertIn("BLOCKED", result.stderr)
        self.assertNotIn("exempt=true", result.stderr)
        self.assertIn(reason, result.stderr)

    def test_bot_release_pr_is_exempt(self) -> None:
        self.open_pull(RELEASE_REF)

        result = self.run_hook()

        self.assert_exempt(result)

    def test_release_bot_app_release_pr_is_exempt(self) -> None:
        self.open_pull(RELEASE_REF, author={"is_bot": True, "login": "app/vmafx-release-bot"})

        self.assert_exempt(self.run_hook())

    def test_human_pr_with_release_head_ref_is_validated(self) -> None:
        self.open_pull(RELEASE_REF, author=HUMAN)

        self.assert_validated(
            self.run_hook(),
            f"exempt=false (head ref '{RELEASE_REF}' looks like a release branch"
            " but author 'lusoris' is not a bot",
        )

    def test_pat_release_pr_with_release_diff_is_exempt(self) -> None:
        self.open_pull(RELEASE_REF, author=HUMAN)
        (self.root / ".release-please-manifest.json").write_text(
            '{"packages": {".": "1.0.0-rc.2"}}\n'
        )
        self.git("add", ".release-please-manifest.json")
        self.git("commit", "-m", "chore(master): release 1.0.0-rc.2")

        result = self.run_hook()

        self.assert_exempt(result)

    def test_pat_release_pr_with_dirty_diff_is_validated(self) -> None:
        self.open_pull(RELEASE_REF, author=HUMAN)
        (self.root / "core").mkdir(exist_ok=True)
        (self.root / "core/libvmaf.c").write_text("int x = 1;\n")
        self.git("add", "core/libvmaf.c")
        self.git("commit", "-m", "feat: sneak in code")

        result = self.run_hook()

        self.assert_validated(
            result,
            "PAT release diff not satisfied: touches non-release file 'core/libvmaf.c'",
        )

    def test_release_looking_pr_from_non_bot_author_is_validated(self) -> None:
        # Same head ref, same stub body; only the author identity differs.
        # None of these may be read as a bot.
        authors = {
            "human login named like the bot": {
                "is_bot": False,
                "login": "github-actions",
                "id": "U_kgDOAAAAAg",
                "name": "Imitator",
            },
            "app/ login without the bot flag": {
                "is_bot": False,
                "login": "app/github-actions",
                "id": "U_kgDOAAAAAw",
                "name": "Imitator",
            },
            "bot flag that is not a JSON boolean": {
                "is_bot": "true",
                "login": "app/github-actions",
            },
            "bot flag without the app/ login shape": {
                "is_bot": True,
                "login": "github-actions",
            },
            # gh marshals a null GraphQL author (deleted account) this way.
            "deleted account": {"is_bot": True, "login": "app/"},
            "null author": None,
            "author missing": ABSENT,
        }
        self.git("switch", "-qc", RELEASE_REF)
        for label, author in authors.items():
            with self.subTest(label):
                self.serve_pull(RELEASE_REF, author=author)

                self.assert_validated(
                    self.run_hook(),
                    f"exempt=false (head ref '{RELEASE_REF}' looks like a release branch",
                )

    def test_numeric_branch_resolving_to_the_release_pr_is_validated(self) -> None:
        # gh reads a numeric branch name as a PR number, so a local branch
        # named after the release PR's number finds the release PR itself.
        self.git("switch", "-qc", "1213")
        self.serve_pull("1213", headRefName=RELEASE_REF)

        result = self.run_hook()

        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(VALIDATOR_MARKER, result.stderr)
        self.assertIn("BLOCKED", result.stderr)
        self.assertIn("is not the local branch", result.stderr)

    def test_bot_pr_missing_head_ref_is_validated(self) -> None:
        self.open_pull(RELEASE_REF, headRefName=ABSENT)

        self.assert_validated(self.run_hook(), "exempt=false (ordinary pull request")

    def test_bot_pr_from_ordinary_branch_is_validated(self) -> None:
        self.open_pull("renovate/anyio-4.x", author={"is_bot": True, "login": "app/renovate"})

        self.assert_validated(self.run_hook(), "exempt=false (ordinary pull request")

    def test_missing_predicate_fails_closed(self) -> None:
        # Branches that predate ADR-1151 have no predicate to consult.
        (self.root / "scripts/ci/release-pr-exempt.sh").unlink()
        self.open_pull(RELEASE_REF)

        self.assert_validated(self.run_hook(), "release-pr-exempt.sh not found")

    def test_public_page_fallback_is_never_exempt(self) -> None:
        # GitHub's public PR page carries no author identity, so the
        # fallback can never prove the bot half of the predicate.
        self.git("switch", "-qc", RELEASE_REF)
        self.write_tool("gh", "exit 1\n")
        self.write_public_page_stub(draft=False, body=RELEASE_BODY)

        self.assert_validated(self.run_hook(), "public PR pages carry no author identity")


if __name__ == "__main__":
    unittest.main()
