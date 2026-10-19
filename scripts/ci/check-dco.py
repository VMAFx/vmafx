#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require a Developer Certificate of Origin sign-off on every pull request commit (ADR-2462).

Every non-merge commit in ``BASE..HEAD`` must end with a ``Signed-off-by: Name
<email>`` trailer (what ``git commit -s`` writes), and the e-mail address of
the trailer must be the commit's author or committer address.  A sign-off in
the middle of the message, or one for somebody else, does not count.

Exemptions are narrow and explicit:

* a commit written by an allow-listed bot (``BOT_LOGINS``), in a pull request
  that GitHub itself reports as opened by that bot (``--author`` and
  ``--author-type Bot``).  A human commit pushed onto a bot branch is not
  exempt, and neither is a commit that merely names a bot as its author in a
  pull request opened by a person;
* the machine-generated release pull request, which the caller detects with
  ``scripts/ci/release-pr-exempt.sh`` and passes as ``--release-pr``;
* a pull request created before the cutoff in ``scripts/ci/dco-cutoff.txt``
  (``--created-at``, the rollout of ADR-2462).  Without ``--created-at`` the
  gate always enforces.

Usage::

    python3 scripts/ci/check-dco.py --base <sha> --head <sha> \\
        [--author <login> --author-type <User|Bot>] [--release-pr] \\
        [--created-at <ISO 8601>] [--cutoff-file <path>]

``BASE_SHA``, ``HEAD_SHA``, ``PR_AUTHOR``, ``PR_AUTHOR_TYPE`` and
``DCO_RELEASE_PR`` and ``PR_CREATED_AT`` are read when the flags are absent.  Exit status: 0 every
commit is signed off or exempt, 1 at least one is not, 2 usage or git error.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

GIT = shutil.which("git") or "/usr/bin/git"
GIT_TIMEOUT_S = 120
DEFAULT_CUTOFF_FILE = Path(__file__).resolve().parent / "dco-cutoff.txt"
MAX_COMMITS = 5000

# GitHub logins (as the API reports them) whose commits are exempt.  Keep the
# list in docs/development/dco.md in step with this one.
BOT_LOGINS = ("renovate[bot]", "dependabot[bot]", "github-actions[bot]")

SIGNOFF_RE = re.compile(r"^signed-off-by:\s+(.+?)\s+<([^<>\s]+@[^<>\s]+)>\s*$", re.IGNORECASE)
FIELD_SEP = "\x1f"
RECORD_SEP = "\x1e"
LOG_FIELDS = 5  # sha, author e-mail, committer e-mail, subject, body


class UsageError(Exception):
    """Raised when git cannot answer; the gate must not read that as a pass."""


@dataclass(frozen=True)
class Commit:
    sha: str
    author_email: str
    committer_email: str
    subject: str
    message: str


def run_git(args: list[str], stdin: str | None = None) -> str:
    """Run git and return stdout; any failure is a UsageError, never an empty answer."""
    try:
        done = subprocess.run(  # noqa: S603 - fixed argv, no shell
            [GIT, *args],
            input=stdin,
            capture_output=True,
            text=True,
            check=False,
            timeout=GIT_TIMEOUT_S,
        )
    except (OSError, subprocess.TimeoutExpired) as exc:
        raise UsageError(f"git {' '.join(args)}: {exc}") from exc
    if done.returncode != 0:
        raise UsageError(f"git {' '.join(args)} exited {done.returncode}: {done.stderr.strip()}")
    return done.stdout


def list_commits(base: str, head: str) -> list[Commit]:
    """Return the non-merge commits of base..head, oldest first."""
    fmt = FIELD_SEP.join(["%H", "%ae", "%ce", "%s", "%B"]) + RECORD_SEP
    out = run_git(["log", "--no-merges", "--reverse", f"--format={fmt}", f"{base}..{head}"])
    commits: list[Commit] = []
    for record in out.split(RECORD_SEP):
        if not record.strip():
            continue
        fields = record.lstrip("\n").split(FIELD_SEP, LOG_FIELDS - 1)
        if len(fields) != LOG_FIELDS:
            raise UsageError(f"unparseable git log record: {record[:80]!r}")
        commits.append(Commit(fields[0], fields[1], fields[2], fields[3], fields[4]))
    if len(commits) > MAX_COMMITS:
        raise UsageError(f"{len(commits)} commits exceed the bound of {MAX_COMMITS}")
    return commits


def trailer_lines(message: str) -> list[str]:
    """Return the trailer block of a commit message, as git parses it."""
    out = run_git(["interpret-trailers", "--parse", "--only-trailers"], stdin=message)
    return [line for line in out.splitlines() if line.strip()]


def signoff_emails(message: str) -> list[str]:
    """Return the e-mail addresses of the well-formed sign-off trailers."""
    found = []
    for line in trailer_lines(message):
        match = SIGNOFF_RE.match(line)
        if match:
            found.append(match.group(2).lower())
    return found


def bot_noreply_suffix(login: str) -> str:
    return f"+{login}@users.noreply.github.com".lower()


def is_exempt_bot_commit(commit: Commit, author: str, author_type: str) -> bool:
    """A bot commit is exempt only inside a pull request opened by that same bot."""
    if author_type != "Bot" or author not in BOT_LOGINS:
        return False
    return commit.author_email.lower().endswith(bot_noreply_suffix(author))


def judge(commit: Commit, author: str, author_type: str) -> str | None:
    """Return None when the commit passes, else the reason it does not."""
    if is_exempt_bot_commit(commit, author, author_type):
        return None
    emails = signoff_emails(commit.message)
    if not emails:
        return "no 'Signed-off-by: Name <email>' trailer"
    own = {commit.author_email.lower(), commit.committer_email.lower()}
    if own.isdisjoint(emails):
        return (
            f"sign-off ({', '.join(emails)}) matches neither the author nor the committer address"
        )
    return None


def report(failures: list[tuple[Commit, str]]) -> None:
    for commit, reason in failures:
        print(f"::error title=DCO sign-off::{commit.sha[:10]} {commit.subject}: {reason}")
    print("")
    print(f"dco: {len(failures)} commit(s) without a valid sign-off.")
    print("  Add the sign-off to each commit of the branch, then push again:")
    print("    git rebase --signoff $(git merge-base origin/master HEAD)")
    print("  New commits: git commit -s   (keep the trailers when squashing)")
    print("  What the sign-off certifies: CONTRIBUTING.md#developer-certificate-of-origin")


def parse_time(text: str, what: str) -> datetime:
    """Parse an ISO 8601 timestamp; a naive one is read as UTC."""
    try:
        value = datetime.fromisoformat(text.strip())
    except ValueError as exc:
        raise UsageError(f"{what}: not an ISO 8601 timestamp: {text.strip()!r}") from exc
    return value if value.tzinfo else value.replace(tzinfo=timezone.utc)


def read_cutoff(path: Path) -> datetime:
    """Return the rollout cutoff: the one non-comment line of the cutoff file."""
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except OSError as exc:
        raise UsageError(f"cutoff file {path}: {exc}") from exc
    values = [ln for ln in lines if ln.strip() and not ln.lstrip().startswith("#")]
    if len(values) != 1:
        raise UsageError(f"cutoff file {path}: expected exactly one timestamp line")
    return parse_time(values[0], f"cutoff file {path}")


def grandfathered(created_at: str, cutoff_file: Path) -> bool:
    """True when the pull request was created strictly before the cutoff."""
    if not created_at:
        return False
    return parse_time(created_at, "created-at") < read_cutoff(cutoff_file)


def check(args: argparse.Namespace) -> int:
    base, head = args.base, args.head
    if args.release_pr:
        print("dco: machine-generated release pull request, exempt.")
        return 0
    if grandfathered(args.created_at, Path(args.cutoff_file)):
        print(f"dco: pull request created {args.created_at}, before the cutoff; grandfathered.")
        return 0
    commits = list_commits(base, head)
    failures = []
    for commit in commits:
        reason = judge(commit, args.author, args.author_type)
        if reason is not None:
            failures.append((commit, reason))
    if failures:
        report(failures)
        return 1
    print(f"dco: PASS - {len(commits)} commit(s), every one signed off or exempt.")
    return 0


def parse_args(argv: list[str]) -> argparse.Namespace:
    env = os.environ.get
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--base", default=env("BASE_SHA", ""))
    parser.add_argument("--head", default=env("HEAD_SHA", ""))
    parser.add_argument("--author", default=env("PR_AUTHOR", ""))
    parser.add_argument("--author-type", default=env("PR_AUTHOR_TYPE", ""))
    parser.add_argument("--created-at", default=env("PR_CREATED_AT", ""))
    parser.add_argument("--cutoff-file", default=str(DEFAULT_CUTOFF_FILE))
    parser.add_argument(
        "--release-pr", action="store_true", default=env("DCO_RELEASE_PR", "") == "true"
    )
    return parser.parse_args(argv)


def main(argv: list[str]) -> int:
    args = parse_args(argv)
    if not args.base or not args.head:
        print(
            "check-dco: --base and --head (or BASE_SHA and HEAD_SHA) are required", file=sys.stderr
        )
        return 2
    try:
        return check(args)
    except UsageError as exc:
        print(f"check-dco: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
