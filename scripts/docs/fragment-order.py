#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""List the files of a fragment directory in the order they landed on the branch.

The ADR index and the rebase notes are rendered from per-PR fragment files
(ADR-2197). Their order is the order the fragments reached the branch: oldest
first, which is what appending a line to a shared manifest used to record. The
order is read from history (the commit that first added each file), so a pull
request names no position and two pull requests never edit the same line.

A file git does not know yet (a fragment in a working tree that is not
committed) sorts last, in name order, so a local render shows it. A history
that cannot answer (a shallow clone) is an error, never a silent fallback: a
render that guessed the order would rewrite the index differently on the next
full clone.

Usage:
    fragment-order.py <directory> [--glob PATTERN] [--exclude PREFIX ...]
        prints one file name per line, oldest first.
"""

from __future__ import annotations

import argparse
import fnmatch
import shutil
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

GIT_TIMEOUT_S = 120
_GIT_PATH = shutil.which("git")
GIT = str(Path(_GIT_PATH).resolve(strict=True)) if _GIT_PATH is not None else None


class OrderError(RuntimeError):
    """The landing order cannot be read."""


def _git(root: Path, *args: str) -> str:
    if GIT is None:
        raise OrderError("required executable not found: git")
    result = run_command(
        [GIT, "-C", str(root), *args],
        allowed_executables=(GIT,),
        capture_output=True,
        text=True,
        timeout_seconds=GIT_TIMEOUT_S,
    )
    if result.returncode != 0:
        raise OrderError(f"git {' '.join(args)}: {result.stderr.strip()}")
    return str(result.stdout)


def landing_order(root: Path, directory: Path) -> list[str]:
    """Paths (relative to ``root``) under ``directory`` in the order commits first added them."""
    if _git(root, "rev-parse", "--is-shallow-repository").strip() == "true":
        raise OrderError(
            "the history is shallow: the landing order of the fragments cannot be read "
            "(fetch full history, for example `git fetch --unshallow`)"
        )
    relative = directory.relative_to(root).as_posix()
    out = _git(
        root,
        "log",
        "--reverse",
        "--no-renames",
        "--diff-filter=A",
        "--format=",
        "--name-only",
        "--",
        relative,
    )
    seen: dict[str, None] = {}
    for line in out.splitlines():
        if line and line not in seen:
            seen[line] = None
    return list(seen)


def ordered_files(
    root: Path, directory: Path, pattern: str = "*.md", exclude_prefixes: tuple[str, ...] = ("_",)
) -> list[Path]:
    """Files of ``directory`` matching ``pattern``: landed ones oldest first, then the rest by name."""
    present = {
        p.name: p
        for p in directory.iterdir()
        if p.is_file()
        and fnmatch.fnmatch(p.name, pattern)
        and not p.name.startswith(exclude_prefixes)
    }
    landed = [
        present[Path(name).name]
        for name in landing_order(root, directory)
        if Path(name).parent == directory.relative_to(root) and Path(name).name in present
    ]
    known = {p.name for p in landed}
    rest = sorted((p for n, p in present.items() if n not in known), key=lambda p: p.name)
    return landed + rest


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("directory", type=Path)
    parser.add_argument("--glob", default="*.md")
    parser.add_argument("--exclude", action="append", default=None, metavar="PREFIX")
    args = parser.parse_args(argv)
    directory = args.directory.resolve()
    root = Path(_git(directory, "rev-parse", "--show-toplevel").strip()).resolve()
    exclude = tuple(args.exclude) if args.exclude is not None else ("_",)
    try:
        for path in ordered_files(root, directory, args.glob, exclude):
            print(path.name)
    except OrderError as exc:
        print(f"fragment-order: {exc}", file=sys.stderr)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
