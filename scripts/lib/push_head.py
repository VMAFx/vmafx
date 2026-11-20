# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse pre-push checks against a checkout other than the outgoing commit."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from scripts.lib import git_env
from scripts.lib.safe_subprocess import run as run_command


def outgoing_head(head: str, root: Path) -> None:
    """Require push framework refs to resolve to the checkout being validated."""
    outgoing = os.environ.get("PRE_COMMIT_TO_REF", os.environ.get("PRE_COMMIT_LOCAL_BRANCH"))
    if outgoing is None:
        return
    git = shutil.which("git")
    if git is None:
        raise RuntimeError("git is required")
    result = run_command(
        [
            git,
            "-C",
            str(root),
            "rev-parse",
            "--verify",
            "--end-of-options",
            f"{outgoing}^{{commit}}",
        ],
        allowed_executables=(git,),
        env=git_env.drop(),
        capture_output=True,
        text=True,
        check=False,
        timeout_seconds=60,
    )
    if result.returncode != 0:
        raise RuntimeError(f"cannot resolve outgoing ref: {result.stderr.strip()}")
    if result.stdout.strip() != head:
        raise RuntimeError("outgoing ref differs from HEAD; check out the branch being pushed")
