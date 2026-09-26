#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contract tests for the repository-local Codex hook configuration."""

from __future__ import annotations

import json
import os
import sys
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

HOOK_CONFIG = REPO_ROOT / ".codex" / "hooks.json"
HOOK_DIR = REPO_ROOT / ".codex" / "hooks"
EXPECTED_HOOKS = {
    ("PreToolUse", "Bash", "block-unsafe-bash.sh"),
    ("PostToolUse", "Edit|Write", "auto-format-on-edit.sh"),
    ("PostToolUse", "Edit|Write", "auto-snapshot-warn.sh"),
    ("PostToolUse", "Edit|Write", "docs-drift-warn.sh"),
    ("PostToolUse", "Edit|Write", "compile-commands-sync.sh"),
    ("SessionStart", None, "session-start.sh"),
    ("Stop", None, "stop.sh"),
}


def repo_root_command(script: str) -> str:
    """Return the only portable command form accepted by this contract."""
    git_env = (
        "env -u GIT_DIR -u GIT_WORK_TREE -u GIT_INDEX_FILE -u GIT_COMMON_DIR "
        "-u GIT_OBJECT_DIRECTORY -u GIT_ALTERNATE_OBJECT_DIRECTORIES"
    )
    return f'"$({git_env} git rev-parse --show-toplevel)/.codex/hooks/{script}"'


def configured_hooks() -> list[tuple[str, str | None, str, str]]:
    """Load configured event, matcher, type, and command tuples."""
    config = json.loads(HOOK_CONFIG.read_text(encoding="utf-8"))
    configured: list[tuple[str, str | None, str, str]] = []
    for event, groups in config["hooks"].items():
        for group in groups:
            matcher = group.get("matcher")
            for hook in group["hooks"]:
                configured.append((event, matcher, hook["type"], hook["command"]))
    return configured


class CodexHookConfigTest(unittest.TestCase):
    def test_hook_matrix_uses_repo_root_relative_commands(self) -> None:
        configured = configured_hooks()
        expected = {
            (event, matcher, "command", repo_root_command(script))
            for event, matcher, script in EXPECTED_HOOKS
        }
        self.assertEqual(len(configured), len(expected), "duplicate or extra hook entry")
        self.assertSetEqual(set(configured), expected)

    def test_hook_targets_are_tracked_executables(self) -> None:
        for _, _, script in EXPECTED_HOOKS:
            path = HOOK_DIR / script
            result = run_command(
                ["git", "ls-files", "--stage", "--", str(path.relative_to(REPO_ROOT))],
                allowed_executables=("git",),
                cwd=REPO_ROOT,
                check=True,
                capture_output=True,
                text=True,
                timeout_seconds=10,
            )
            self.assertTrue(result.stdout, f"{script} is not tracked")
            self.assertEqual(result.stdout.split()[0], "100755", f"{script} is not executable")

    def test_pre_tool_hook_resolves_from_a_repository_subdirectory(self) -> None:
        command = repo_root_command("block-unsafe-bash.sh")
        poisoned_env = dict(os.environ)
        poisoned_env["GIT_WORK_TREE"] = "."
        result = run_command(
            ["bash", "-c", command],
            allowed_executables=("bash",),
            cwd=REPO_ROOT / "core" / "src",
            input_data='{"tool_input":{"command":"git status --short"}}',
            check=False,
            capture_output=True,
            env=poisoned_env,
            text=True,
            timeout_seconds=10,
        )
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
