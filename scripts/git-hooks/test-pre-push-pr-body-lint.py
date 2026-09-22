#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Exercise the PR-body hook's bounded, fail-closed metadata lookup."""

from __future__ import annotations

import asyncio
import os
import shutil
import tempfile
import unittest
from dataclasses import dataclass
from pathlib import Path

SCRIPT = Path(__file__).with_name("pre-push-pr-body-lint.sh").resolve()
GIT = Path(shutil.which("git") or "/usr/bin/git")


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


class PullRequestLookup(unittest.TestCase):
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

    def write_public_page_stub(self) -> None:
        self.write_tool(
            "curl",
            'case "${*: -1}" in\n'
            "  */pulls) printf '%s\\n' '<a href=\"/VMAFx/vmafx/pull/1514\">PR</a>' ;;\n"
            "  */pull/1514) cat <<'EOF'\n"
            '<div class="js-pull-header-details" data-pull-is-open="true"></div>\n'
            '<span data-status="draft"></span>\n'
            '<clipboard-copy role="menuitem" value="## Body&#10;&#10;- [x] ok"></clipboard-copy>\n'
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


if __name__ == "__main__":
    unittest.main()
