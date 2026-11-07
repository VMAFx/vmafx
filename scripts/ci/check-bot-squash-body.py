#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse a bot pull request whose platform auto-merge will produce a forbidden
Co-authored-by trailer on the squash commit (ADR-3389, fleet-bot-squash-no-coauthor-20261016).

When GitHub squash-merges a multi-author branch with the *default* commit body
(``commit_message`` null in the auto-merge configuration), the forge appends a
``Co-authored-by`` trailer for every distinct author.  On a bot pull request
that a session pushed a fix commit to, that adds a ``[bot]`` Co-authored-by
which the landed-commit-messages gate refuses — but only *after* the commit is
already on master.

This gate catches the pattern *before* the merge fires:

1. The pull request was opened by a bot (``--author-type Bot``).
2. Auto-merge is armed (``--auto-merge-armed``).
3. The auto-merge commit body is null / empty (``--auto-merge-body`` absent or
   empty), meaning the forge will use its default body.
4. At least one non-merge commit on the branch was authored by someone other
   than the pull-request-opening bot (``--base`` / ``--head`` commit range).

All four must hold for a failure.  A bot branch with only bot commits is fine
(the forge's default body lists only the bot).  A branch with an explicit commit
body is fine (the forge uses it as-is and does not append trailers).

Environment variables ``PR_AUTHOR``, ``PR_AUTHOR_TYPE``, ``BASE_SHA``,
``HEAD_SHA``, ``AUTO_MERGE_ARMED`` and ``AUTO_MERGE_BODY`` are read when the
corresponding flags are absent.

Exit: 0 pass, 1 refused, 2 usage error.
"""

from __future__ import annotations

import argparse
import os
import shutil
import subprocess
import sys

GIT = shutil.which("git") or "/usr/bin/git"
GIT_TIMEOUT_S = 60

BOT_LOGINS = (
    "renovate[bot]",
    "dependabot[bot]",
    "github-actions[bot]",
    "vmafx-release-bot[bot]",
    "plurano-merge-train[bot]",
)


class GitError(Exception):
    """Raised when git cannot answer; the gate must not read that as a pass."""


def run_git(args: list[str]) -> str:
    try:
        done = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [GIT, *args],
            capture_output=True,
            text=True,
            check=False,
            timeout=GIT_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise GitError(f"git {' '.join(args)}: {exc}") from exc
    if done.returncode != 0:
        raise GitError(f"git {' '.join(args)} exited {done.returncode}: {done.stderr.strip()}")
    return done.stdout


def bot_noreply_suffix(login: str) -> str:
    return f"+{login}@users.noreply.github.com".lower()


def has_non_bot_commit(base: str, head: str, bot_login: str) -> bool:
    """True if any non-merge commit in base..head was authored by someone other than bot_login."""
    suffix = bot_noreply_suffix(bot_login)
    out = run_git(["log", "--no-merges", "--format=%ae", f"{base}..{head}"])
    for raw_email in out.splitlines():
        addr = raw_email.strip().lower()
        if addr and not addr.endswith(suffix):
            return True
    return False


def parse_args(argv: list[str]) -> argparse.Namespace:
    env = os.environ.get
    desc = (__doc__ or "").split("\n", 1)[0]
    p = argparse.ArgumentParser(description=desc)
    p.add_argument("--author", default=env("PR_AUTHOR", ""))
    p.add_argument("--author-type", default=env("PR_AUTHOR_TYPE", ""))
    p.add_argument("--base", default=env("BASE_SHA", ""))
    p.add_argument("--head", default=env("HEAD_SHA", ""))
    p.add_argument("--auto-merge-armed", default=env("AUTO_MERGE_ARMED", ""))
    p.add_argument("--auto-merge-body", default=env("AUTO_MERGE_BODY", ""))
    return p.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)

    if not args.base or not args.head:
        print("check-bot-squash-body: --base and --head required", file=sys.stderr)
        return 2

    if args.author_type != "Bot":
        print("check-bot-squash-body: PASS — not a bot pull request.")
        return 0

    if args.auto_merge_armed.lower() not in ("true", "1", "yes"):
        print("check-bot-squash-body: PASS — auto-merge not armed.")
        return 0

    if args.auto_merge_body.strip():
        print("check-bot-squash-body: PASS — auto-merge has an explicit commit body.")
        return 0

    if not has_non_bot_commit(args.base, args.head, args.author):
        print("check-bot-squash-body: PASS — all commits are by the opening bot.")
        return 0

    print(
        "::error title=Bot squash body::"
        f"This bot pull request (by {args.author}) has auto-merge armed with "
        "the platform default commit body (null), and contains commits by a "
        "non-bot author.  GitHub's default squash body will append "
        "Co-authored-by trailers that include a [bot] identity, which the "
        "landed-commit-messages gate refuses on master."
    )
    print("")
    print("  Fix: set an explicit squash commit body on the auto-merge configuration")
    print("  (gh pr merge --squash --body <body>), or disable auto-merge, squash-merge")
    print("  manually with an explicit body, and re-arm auto-merge for the next run.")
    print("")
    print(
        "  Defence: fleet-bot-squash-no-coauthor-20261016; "
        "recurrence path: platform auto-merge on a bot PR with a hand commit."
    )
    return 1


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv[1:]))
    except GitError as exc:
        print(f"check-bot-squash-body: {exc}", file=sys.stderr)
        sys.exit(2)
