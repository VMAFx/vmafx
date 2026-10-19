#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check the rendered changelog, ADR index and rebase notes on a master push (ADR-3295).

A pull request carries fragments, never the render (ADR-2197). Since ADR-3295
the render lands as its own render-only pull request after a batch, so master
may sit between a landing and its render for a while. This check keeps that
window bounded and the last render exact:

1. ``make docs-render-check`` passes at HEAD: green.
2. Otherwise find the last first-parent commit that touched a rendered file.
   Its own tree must pass ``make docs-render-check``: a render that is not the
   render of its fragments (a hand edit, a stale render) is red.
3. The first first-parent commit after it starts the owed render. Older than
   the bound (24 hours by default): red. Within it: green with a warning.

Usage: check_render_on_master.py [--root DIR] [--bound-hours H] [--now EPOCH]
"""

from __future__ import annotations

import argparse
import os
import shutil
import sys
import tempfile
import time
from dataclasses import dataclass
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

# The files `make docs-render` writes; deliverables-check.sh keeps the same list
# (`rendered_paths`), without `_order.txt`, which no render writes.
RENDERED_PATHS = (
    "CHANGELOG.md",
    "docs/adr/README.md",
    "docs/adr/titles.md",
    "docs/research/titles.md",
    "docs/adr/by-tag",
    "docs/rebase-notes.md",
)
GIT_TIMEOUT_S = 120
RENDER_TIMEOUT_S = 900
DEFAULT_BOUND_HOURS = 24.0
TITLE = "ADR-3295 rendered docs"


@dataclass(frozen=True)
class Verdict:
    """The outcome: exit status and the annotation lines to print."""

    status: int
    lines: tuple[str, ...]


def git_environment() -> dict[str, str]:
    """The caller's environment without GIT_* variables, which would point git
    at another repository (a hook's, a test's)."""
    return {key: value for key, value in os.environ.items() if not key.startswith("GIT_")}


def git(root: Path, *args: str) -> str:
    """Run git in `root` and return stdout; a failure raises."""
    result = run_command(
        ["git", "-C", str(root), *args],
        allowed_executables=("git",),
        env=git_environment(),
        capture_output=True,
        text=True,
        check=True,
        timeout_seconds=GIT_TIMEOUT_S,
    )
    return str(result.stdout)


def render_check(tree: Path) -> tuple[bool, str]:
    """Run `make docs-render-check` in `tree`; return (passed, output)."""
    result = run_command(
        ["make", "-s", "-C", str(tree), "docs-render-check"],
        allowed_executables=("make",),
        env=git_environment(),
        capture_output=True,
        stderr_to_stdout=True,
        text=True,
        check=False,
        timeout_seconds=RENDER_TIMEOUT_S,
    )
    return result.returncode == 0, str(result.stdout).strip()


def render_check_at(root: Path, commit: str) -> tuple[bool, str]:
    """Run the render check on the tree of `commit` in a disposable worktree."""
    scratch = Path(tempfile.mkdtemp(prefix="render-check-"))
    tree = scratch / "tree"
    try:
        git(root, "worktree", "add", "--detach", "--quiet", str(tree), commit)
        return render_check(tree)
    finally:
        run_command(
            ["git", "-C", str(root), "worktree", "remove", "--force", str(tree)],
            allowed_executables=("git",),
            env=git_environment(),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=GIT_TIMEOUT_S,
        )
        shutil.rmtree(scratch, ignore_errors=True)
        git(root, "worktree", "prune")


def last_render_commit(root: Path) -> str:
    """The last first-parent commit of HEAD that touched a rendered file, or ''."""
    out = git(
        root, "log", "--first-parent", "-n", "1", "--format=%H", "HEAD", "--", *RENDERED_PATHS
    )
    return out.strip()


def first_commit_after(root: Path, commit: str) -> tuple[str, int] | None:
    """The first first-parent commit after `commit` and its committer time."""
    out = git(root, "log", "--first-parent", "--reverse", "--format=%H %ct", f"{commit}..HEAD")
    lines = out.splitlines()
    if not lines:
        return None
    sha, stamp = lines[0].split()
    return sha, int(stamp)


def judge_owed(root: Path, render: str, now: float, bound_hours: float) -> Verdict:
    """Steps 2 and 3: the last render is exact and the owed one is young enough."""
    exact, output = render_check_at(root, render)
    if not exact:
        return Verdict(
            1,
            (
                output,
                f"::error title={TITLE}::the last render commit {render[:12]} is not the "
                "render of its fragments (make docs-render-check fails on its tree)",
            ),
        )
    first = first_commit_after(root, render)
    if first is None:
        return Verdict(
            1, (f"::error title={TITLE}::HEAD is the last render commit and still fails",)
        )
    sha, stamp = first
    age_hours = (now - stamp) / 3600.0
    when = time.strftime("%Y-%m-%d %H:%M UTC", time.gmtime(stamp))
    owed = f"render owed since {sha[:12]} ({when}, {age_hours:.1f} h ago)"
    if age_hours > bound_hours:
        return Verdict(1, (f"::error title={TITLE}::{owed}; the bound is {bound_hours:g} h",))
    return Verdict(0, (f"::warning title={TITLE}::{owed}; within the {bound_hours:g} h bound",))


def judge(root: Path, now: float, bound_hours: float) -> Verdict:
    """The whole check for the HEAD of `root`."""
    passed, output = render_check(root)
    if passed:
        return Verdict(0, ("check_render_on_master: the render at HEAD is current",))
    render = last_render_commit(root)
    if not render:
        return Verdict(1, (output, f"::error title={TITLE}::no commit of HEAD carries a render"))
    verdict = judge_owed(root, render, now, bound_hours)
    return Verdict(verdict.status, (output, *verdict.lines))


def parse_args(argv: list[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=REPO_ROOT)
    parser.add_argument("--bound-hours", type=float, default=DEFAULT_BOUND_HOURS)
    parser.add_argument("--now", type=float, default=None, help="epoch seconds (tests)")
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    now = time.time() if args.now is None else args.now
    verdict = judge(args.root.resolve(), now, args.bound_hours)
    for line in verdict.lines:
        if line:
            print(line)
    return verdict.status


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
