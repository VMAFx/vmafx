# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Disposable Git repository fixture helpers for docs test suites."""

from __future__ import annotations

import shutil
from pathlib import Path

from scripts.lib import git_env
from scripts.lib.safe_subprocess import run as run_command


def git_init(root: Path) -> None:
    """Make the fixture a repository: the fragment order is read from its history."""
    git = shutil.which("git")
    assert git is not None, "git is required by the generator fixture"
    run_command(
        (git, "init", "-q", str(root)),
        allowed_executables=(git,),
        # Without the hook's GIT_DIR: the fixture, not the caller's repository.
        env=git_env.hermetic(),
        capture_output=True,
        check=True,
        timeout_seconds=60,
    )
