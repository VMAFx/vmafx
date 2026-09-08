#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-2-Clause-Patent
"""Type-check all branch-owned ai/scripts Python files against the master merge base.

An old remote tip is not a PR base after a rebase: its diff includes unrelated
changes already integrated on master. Preserve the touched-file scope in AGENTS.md.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from pathlib import Path


def git(*args: str) -> str:
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("git is required")
    result = subprocess.run(  # noqa: S603 -- literal git operations, argument vector
        [executable, *args], capture_output=True, text=True, check=True
    )
    return result.stdout


def main() -> int:
    try:
        root = Path(git("rev-parse", "--show-toplevel").strip()).resolve()
        head = git("rev-parse", "--verify", "HEAD^{commit}").strip()
        # The all-files first-push path exposes only LOCAL_BRANCH. Normal
        # pre-push runs expose TO_REF, which is the exact outgoing commit.
        outgoing = os.environ.get("PRE_COMMIT_TO_REF", os.environ.get("PRE_COMMIT_LOCAL_BRANCH"))
        if outgoing is not None:
            target = git(
                "rev-parse", "--verify", "--end-of-options", f"{outgoing}^{{commit}}"
            ).strip()
            if target != head:
                raise RuntimeError(
                    "outgoing ref differs from HEAD; check out the branch being pushed"
                )
        base = git("merge-base", "origin/master", head).strip()
        changed = git(
            "diff", "--name-only", "-z", "--diff-filter=ACMRT", base, head, "--", "ai/", "scripts/"
        )
        selected = sorted(
            path
            for path in changed.split("\0")
            if path.startswith(("ai/", "scripts/")) and path.endswith(".py")
        )
        for filename in selected:
            # Keep Git's lexical filename as the identity passed to mypy.
            # Resolving for selection would silently omit newly added symlinks
            # to unchanged files. Resolution is only a containment/type check.
            path = (root / filename).resolve(strict=True)
            path.relative_to(root)
            if not path.is_file():
                raise RuntimeError(f"selected Python path is not a regular file: {filename}")
        executable = shutil.which("mypy")
        if executable is None:
            raise RuntimeError("mypy is required; install the local type-checking toolchain")
        if not selected:
            print("mypy: no ai/scripts Python files differ from the branch's master merge base")
            return 0
        return subprocess.run(  # noqa: S603 -- contained Git filenames, no shell
            [executable, *selected], cwd=root, check=False
        ).returncode
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as exc:
        print(f"mypy scope check failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
