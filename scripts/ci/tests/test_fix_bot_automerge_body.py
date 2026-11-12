#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for fix-bot-automerge-body.py."""

from __future__ import annotations

import importlib.util
import json
import os
import subprocess
import sys
import types
from pathlib import Path
from unittest.mock import MagicMock, patch

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


def _load_module() -> types.ModuleType:
    spec = importlib.util.spec_from_file_location("fix_bot", SCRIPT)
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


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


class TestCallMutation:
    """Test _call_mutation via mocked subprocess."""

    def _make_response(self, body: str | None) -> str:
        return json.dumps(
            {
                "data": {
                    "enablePullRequestAutoMerge": {
                        "pullRequest": {"autoMergeRequest": {"commitBody": body}}
                    }
                }
            }
        )

    def test_success_returns_body(self) -> None:
        mod = _load_module()
        signoff = "Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>"
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = self._make_response(signoff)
        mock_result.stderr = ""
        with patch.object(subprocess, "run", return_value=mock_result):
            rc, body = mod._call_mutation("NODE1", "chore(deps): update (#99)", signoff)
        assert rc == 0
        assert body == signoff

    def test_metacharacter_headline_reaches_the_mutation_unchanged(self) -> None:
        """Issue #3348: a title with quoting or shell metacharacters reaches the
        mutation verbatim. The headline travels as one gh argv element, never
        through a shell, and a trailing newline survives.
        """
        mod = _load_module()
        title = 'chore(deps): update "lock" `pin` $(echo pwned) \\ end\n'
        signoff = (
            "Signed-off-by: renovate[bot] " "<29139614+renovate[bot]@users.noreply.github.com>"
        )
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = self._make_response(signoff)
        mock_result.stderr = ""
        bot_env = {
            "PR_NUMBER": "3047",
            "PR_NODE_ID": "PR_fakenode123",
            "PR_TITLE": title,
            "PR_AUTHOR": "renovate[bot]",
            "PR_AUTHOR_TYPE": "Bot",
            "PR_AUTHOR_ID": "29139614",
            "AUTO_MERGE_BODY": "",
        }
        with (
            patch.object(subprocess, "run", return_value=mock_result) as run,
            patch.dict(os.environ, bot_env),
        ):
            rc = mod.main()
        assert rc == 0
        argv = run.call_args.args[0]
        assert isinstance(argv, list), "gh must run from an argv list, not a shell string"
        assert not run.call_args.kwargs.get("shell", False)
        field = f"headline={title} (#3047)"
        assert field in argv
        assert argv[argv.index(field) - 1] == "-f"

    def test_gh_nonzero_exit(self) -> None:
        mod = _load_module()
        mock_result = MagicMock()
        mock_result.returncode = 1
        mock_result.stdout = ""
        mock_result.stderr = "gh: error"
        with patch.object(subprocess, "run", return_value=mock_result):
            rc, body = mod._call_mutation("NODE1", "title", "signoff")
        assert rc == 1
        assert body is None

    def test_timeout_returns_error(self) -> None:
        mod = _load_module()
        with patch.object(subprocess, "run", side_effect=subprocess.TimeoutExpired("gh", 30)):
            rc, body = mod._call_mutation("NODE1", "title", "signoff")
        assert rc == 1
        assert body is None

    def test_oserror_returns_error(self) -> None:
        mod = _load_module()
        with patch.object(subprocess, "run", side_effect=OSError("gh not found")):
            rc, body = mod._call_mutation("NODE1", "title", "signoff")
        assert rc == 1
        assert body is None

    def test_malformed_json_returns_none_body(self) -> None:
        mod = _load_module()
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = "not json"
        mock_result.stderr = ""
        with patch.object(subprocess, "run", return_value=mock_result):
            rc, body = mod._call_mutation("NODE1", "title", "signoff")
        assert rc == 0
        assert body is None

    def test_missing_nested_key_returns_none_body(self) -> None:
        mod = _load_module()
        mock_result = MagicMock()
        mock_result.returncode = 0
        mock_result.stdout = json.dumps({"data": {}})
        mock_result.stderr = ""
        with patch.object(subprocess, "run", return_value=mock_result):
            rc, body = mod._call_mutation("NODE1", "title", "signoff")
        assert rc == 0
        assert body is None


class TestMainMutationPath:
    """Test main() through the mutation path (not just skip/usage)."""

    def test_main_rearm_success(self) -> None:
        mod = _load_module()
        signoff = "Signed-off-by: renovate[bot] <29139614+renovate[bot]@users.noreply.github.com>"
        with (
            patch.dict(os.environ, BASE_ENV, clear=False),
            patch.object(
                mod,
                "_call_mutation",
                return_value=(0, signoff),
            ),
        ):
            rc = mod.main()
        assert rc == 0

    def test_main_rearm_mutation_failure(self) -> None:
        mod = _load_module()
        with (
            patch.dict(os.environ, BASE_ENV, clear=False),
            patch.object(
                mod,
                "_call_mutation",
                return_value=(1, None),
            ),
        ):
            rc = mod.main()
        assert rc == 1

    def test_main_rearm_body_missing_signoff(self) -> None:
        mod = _load_module()
        with (
            patch.dict(os.environ, BASE_ENV, clear=False),
            patch.object(
                mod,
                "_call_mutation",
                return_value=(0, "some other body"),
            ),
        ):
            rc = mod.main()
        assert rc == 1
