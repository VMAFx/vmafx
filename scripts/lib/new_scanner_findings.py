# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Compare scanner findings with the merge-base copy of a changed file."""

from __future__ import annotations

import collections
import os
import shutil
import tempfile
from collections.abc import Callable
from pathlib import Path

from scripts.lib.safe_subprocess import run as run_command

Finding = tuple[int, str, str]


class ToolError(RuntimeError):
    """A scanner or git is missing or failed."""


def tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise ToolError(f"{name} is not on PATH")
    return path


def git_output(git: str, args: list[str], cwd: Path) -> tuple[int, str]:
    """Run a read-only git command in the selected repository."""
    # Read-only git in `cwd`; the hook's GIT_DIR / GIT_INDEX_FILE are dropped so
    # `cwd` alone names the repository.
    env = {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}
    proc = run_command(
        [git, *args],
        allowed_executables=(git,),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        timeout_seconds=60,
        max_output_bytes=64 * 1_048_576,
    )
    return proc.returncode, proc.stdout


def merge_base(git: str, root: Path, branch: str) -> str:
    status, output = git_output(git, ["merge-base", "HEAD", branch], root)
    if status != 0 or not output.strip():
        raise ToolError(f"no merge base between HEAD and {branch}")
    return output.strip()


def renames(git: str, root: Path, base: str) -> dict[str, str]:
    """Map each path renamed since base to its original path."""
    status, output = git_output(
        git, ["diff", "-M", "--name-status", "-z", "--diff-filter=R", base], root
    )
    if status != 0:
        raise ToolError(f"git diff against {base} failed")
    fields = output.split("\0")
    return {fields[index + 2]: fields[index + 1] for index in range(0, len(fields) - 2, 3)}


def base_text(git: str, root: Path, base: str, relative: str) -> str:
    """Return the file at base, or empty text when it did not exist there."""
    status, names = git_output(git, ["ls-tree", "-z", "--name-only", base, "--", relative], root)
    if status != 0:
        raise ToolError(f"git ls-tree against {base} failed")
    if relative not in names.split("\0"):
        return ""
    status, output = git_output(git, ["show", f"{base}:{relative}"], root)
    if status != 0:
        raise ToolError(f"git show {base}:{relative} failed")
    return output


def new_findings(
    scan: Callable[[Path], list[Finding]], current: Path, old_text: str
) -> list[Finding]:
    """Return findings absent at base; skip both scans when the bytes match.

    Bytes, not decoded text: a file that is not UTF-8 still reaches the scanner.
    """
    if current.read_bytes() == old_text.encode("utf-8"):
        return []
    with tempfile.TemporaryDirectory(prefix="scanner-base-") as temp:
        old = Path(temp) / current.name
        old.write_text(old_text, encoding="utf-8")
        old_keys = collections.Counter((rule, line) for _, rule, line in scan(old))
    added = []
    for found in scan(current):
        key = (found[1], found[2])
        if old_keys[key] > 0:
            old_keys[key] -= 1
        else:
            added.append(found)
    return added
