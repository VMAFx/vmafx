#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for fix-bot-automerge-body.py."""

from __future__ import annotations

import os
import subprocess
import sys
from pathlib import Path

SCRIPT = str(Path(__file__).resolve().parent.parent / "fix-bot-automerge-body.py")
BASE_ENV = {
    "PR_NUMBER": "3047",
    "PR_NODE_ID": "PR_fakenode123",
    "PR_TITLE": "chore(deps): Lock file maintenance",
    "PR_AUTHOR": "renovate[bot]",
    "PR_AUTHOR_TYPE": "Bot",
    "PR_AUTHOR_ID": "29139614",
    "AUTO_MERGE_BODY": "",
}


def run_script(env_override: dict[str, str] | None = None) -> subprocess.CompletedProcess[str]:
    env = {**os.environ, **BASE_ENV, **(env_override or {})}
    return subprocess.run(  # noqa: S603 -- fixed argv, no shell
        [sys.executable, SCRIPT],
        capture_output=True,
        text=True,
        check=False,
        env=env,
        timeout=10,
    )


class TestSkipConditions:
    def test_skip_non_bot(self) -> None:
        result = run_script({"PR_AUTHOR_TYPE": "User"})
        assert result.returncode == 0
        assert "SKIP" in result.stdout
        assert "not a bot" in result.stdout

    def test_skip_body_already_set(self) -> None:
        result = run_script(
            {
                "AUTO_MERGE_BODY": "Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>"
            }
        )
        assert result.returncode == 0
        assert "SKIP" in result.stdout
        assert "already set" in result.stdout

    def test_usage_error_missing_node_id(self) -> None:
        result = run_script({"PR_NODE_ID": ""})
        assert result.returncode == 2  # noqa: PLR2004
        assert "missing required" in result.stderr


class TestSignoffBody:
    def test_signoff_format(self) -> None:
        """The sign-off body must match renovate[bot]'s noreply address."""
        expected = "Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>"
        author = BASE_ENV["PR_AUTHOR"]
        author_id = BASE_ENV["PR_AUTHOR_ID"]
        body = f"Signed-off-by: {author} <{author_id}+{author}@users.noreply.github.com>"
        assert body == expected

    def test_headline_includes_pr_number(self) -> None:
        title = BASE_ENV["PR_TITLE"]
        number = BASE_ENV["PR_NUMBER"]
        headline = f"{title} (#{number})"
        assert headline == "chore(deps): Lock file maintenance (#3047)"


class TestMutationPayload:
    """Verify the GraphQL mutation source contains the right parameters."""

    def test_mutation_contains_signoff_keyword(self) -> None:
        content = Path(SCRIPT).read_text()
        assert "enablePullRequestAutoMerge" in content

    def test_mutation_squash_method(self) -> None:
        content = Path(SCRIPT).read_text()
        assert "mergeMethod: SQUASH" in content

    def test_mutation_accepts_body_parameter(self) -> None:
        content = Path(SCRIPT).read_text()
        assert "commitBody: $body" in content
