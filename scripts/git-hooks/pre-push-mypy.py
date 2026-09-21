#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run strict mypy over every tracked Python source in ``ai/`` and ``scripts/``.

This is the single type-checking entry point used by Make, pre-push, and hosted
CI.  It deliberately has no merge-base or touched-file mode: a non-zero result
anywhere in the owned scope blocks every caller.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path


@dataclass(frozen=True)
class CheckGroup:
    """Files sharing one import root and therefore one canonical module identity."""

    name: str
    paths: tuple[str, ...]
    search_roots: tuple[str, ...]


def git(root: Path, *args: str) -> str:
    """Run a read-only Git query in *root* and return stdout."""
    executable = shutil.which("git")
    if executable is None:
        raise RuntimeError("git is required")
    result = subprocess.run(  # noqa: S603 -- literal git operations, argument vector
        [executable, "-C", str(root), *args],
        capture_output=True,
        text=True,
        check=True,
    )
    return result.stdout


def repository_root() -> Path:
    """Resolve the checkout that owns the caller's current working directory."""
    return Path(git(Path.cwd(), "rev-parse", "--show-toplevel").strip()).resolve()


def owned_paths(root: Path) -> list[str]:
    """Return every tracked Python source in the two gate-owned trees."""
    tracked = git(root, "ls-files", "-z", "--", "ai", "scripts").split("\0")
    selected = sorted(path for path in tracked if path and Path(path).suffix in {".py", ".pyi"})
    for filename in selected:
        resolved = (root / filename).resolve(strict=True)
        resolved.relative_to(root)
        if not resolved.is_file():
            raise RuntimeError(f"tracked Python path is not a regular file: {filename}")
    return selected


def check_groups(paths: list[str]) -> tuple[CheckGroup, ...]:
    """Partition owned files by the import root their runtime entry point uses."""
    specifications = (
        ("ai-source", "ai/src/", ("ai/src", "ai/typings", "tools/vmaf-tune/src", ".")),
        (
            "ai-scripts",
            "ai/scripts/",
            ("ai/src", "ai/typings", "tools/vmaf-tune/src", "."),
        ),
        (
            "ai-tests",
            "ai/tests/",
            ("ai/src", "ai/typings", "tools/vmaf-tune/src", "."),
        ),
        ("ai-typings", "ai/typings/", ("ai/typings", "ai/src", ".")),
        (
            "ci-scripts",
            "scripts/ci/",
            ("scripts/ci", "scripts", "ai/src", "ai/typings", "."),
        ),
        (
            "development-scripts",
            "scripts/dev/",
            ("scripts/dev", "scripts", "ai/src", "ai/typings", "."),
        ),
        (
            "other-scripts",
            "scripts/",
            ("scripts", "ai/src", "ai/typings", "."),
        ),
    )
    claimed: set[str] = set()
    groups: list[CheckGroup] = []
    for name, prefix, search_roots in specifications:
        selected = tuple(path for path in paths if path.startswith(prefix) and path not in claimed)
        if selected:
            groups.append(CheckGroup(name, selected, search_roots))
            claimed.update(selected)
    remainder = tuple(path for path in paths if path not in claimed)
    if remainder:
        groups.append(
            CheckGroup(
                "repository-packages",
                remainder,
                ("ai/src", "ai/typings", "tools/vmaf-tune/src", "."),
            )
        )
    return tuple(groups)


def python_for_mypy(executable: str) -> str:
    """Return the dependency environment paired with the mypy executable."""
    override = os.environ.get("MYPY_PYTHON_EXECUTABLE")
    if override:
        resolved = shutil.which(override)
        if resolved is None:
            raise RuntimeError(f"MYPY_PYTHON_EXECUTABLE is not executable: {override}")
        return resolved
    executable_dir = Path(executable).parent
    names = ("python.exe", "python3.exe") if os.name == "nt" else ("python", "python3")
    for name in names:
        candidate = executable_dir / name
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return str(candidate)
    return sys.executable


def run_mypy(executable: str, python_executable: str, root: Path, paths: list[str]) -> int:
    """Check each package root once under its canonical import identity."""
    common = [
        executable,
        f"--config-file={root / 'pyproject.toml'}",
        f"--python-executable={python_executable}",
        "--explicit-package-bases",
    ]
    status = 0
    for group in check_groups(paths):
        environment = dict(os.environ)
        environment["MYPYPATH"] = os.pathsep.join(
            str((root / search_root).resolve()) for search_root in group.search_roots
        )
        completed = subprocess.run(  # noqa: S603 -- contained Git filenames, no shell
            [*common, *group.paths],
            cwd=root,
            env=environment,
            check=False,
        )
        if completed.returncode != 0:
            status = completed.returncode
    return status


def main(argv: list[str] | None = None) -> int:
    """Validate the complete scope; command-line filenames cannot narrow it."""
    arguments = sys.argv[1:] if argv is None else argv
    if arguments:
        print(
            "mypy scope check does not accept filenames; it always checks all owned files",
            file=sys.stderr,
        )
        return 2
    try:
        root = repository_root()
        paths = owned_paths(root)
        if not paths:
            raise RuntimeError("no tracked Python files found under ai/ or scripts/")
        executable = shutil.which("mypy")
        if executable is None:
            raise RuntimeError("mypy is required; run `make lint-tools`")
        return run_mypy(executable, python_for_mypy(executable), root, paths)
    except (OSError, RuntimeError, subprocess.CalledProcessError, ValueError) as exc:
        print(f"mypy scope check failed: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
