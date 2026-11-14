#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Require DCO sign-offs and refuse model/tool attribution in PRs and landed commits.

ADR-2462 covers pull request sign-offs; ADR-3389 covers landed squash commits.
ADR-3531 covers identity and message attribution checks.
ADR-3544 refuses bot-authored PR/pre-push commits unless the bot is the branch opener.

Every non-merge commit in ``BASE..HEAD`` must end with a ``Signed-off-by: Name
<email>`` trailer (what ``git commit -s`` writes), and the e-mail address of
the trailer must be the commit's author or committer address.  A sign-off in
the middle of the message, or one for somebody else, does not count.

On master (``--landed``), all exemptions are disabled.  Every commit must have
a sign-off matching the commit author or the designated landing identity
(``--landing-identity``).  Committer address is not accepted because GitHub is
committer on every squash. Commit author/committer identities must not name a
model or coding tool. Commit messages and pull request bodies must not carry
banned Co-authored-by lines or machine-generated attribution.

Sign-off exemptions for pull requests (attribution checks still run):

* a commit written by an allow-listed bot (``BOT_LOGINS``), in a pull request
  that GitHub itself reports as opened by that bot (``--author`` and
  ``--author-type Bot``).  A human commit pushed onto a bot branch is not
  exempt, and neither is a commit that merely names a bot as its author in a
  pull request opened by a person;
* the machine-generated release pull request, which the caller detects with
  ``scripts/ci/release-pr-exempt.sh`` and passes as ``--release-pr``;
* a pull request created before the cutoff in ``scripts/ci/dco-cutoff.txt``
  (``--created-at``, the rollout of ADR-2462).  Without ``--created-at``
  sign-offs are always enforced.

Usage::

    python3 scripts/ci/check-dco.py --base <sha> --head <sha> \\
        [--author <login> --author-type <User|Bot>] [--release-pr] \\
        [--created-at <ISO 8601>] [--cutoff-file <path>] \
        [--pr-body-file <path> | --pr-body-from-event]

    python3 scripts/ci/check-dco.py --landed --landing-identity "Name <email>" \\
        --base <sha> --head <sha>

    python3 scripts/ci/check-dco.py --merge-base origin/master [--head <rev>]

``--merge-base REF`` is the pre-push mode, named like the other pre-push hooks'
option: it checks the commits the push adds on top of ``REF``, those reachable
from the outgoing commit but not from ``REF``. The outgoing commit is
``--head``, else the ``PRE_COMMIT_TO_REF`` that pre-commit exports during a
push, else ``HEAD``. It catches commits that never passed the local commit-msg
hook, such as a review bot's commit pulled from the pull request branch.

``BASE_SHA``, ``HEAD_SHA``, ``PR_AUTHOR``, ``PR_AUTHOR_TYPE``,
``DCO_RELEASE_PR``, ``PR_CREATED_AT`` and ``LANDING_IDENTITY`` are read when the flags
are absent. ``--merge-base`` does not read ``HEAD_SHA``, so an inherited value
cannot replace the pushed commit. ``--pr-body-from-event`` reads ``GITHUB_EVENT_PATH`` JSON in CI.
Locally, ``--pr-body-file`` (alias ``--pr-body``) reads a UTF-8 file; otherwise
``PR_BODY`` supplies the body. Inputs exceeding named byte/line bounds are refused.
Exit status: 0 all checks pass, 1 attribution or sign-off fails, 2 usage or git error.
"""

from __future__ import annotations

import argparse
import functools
import json
import os
import re
import shutil
import sys
import unicodedata
from collections.abc import Callable
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    run,
)

GIT = shutil.which("git") or "/usr/bin/git"
GIT_TIMEOUT_S = 120
DEFAULT_CUTOFF_FILE = Path(__file__).resolve().parent / "dco-cutoff.txt"
MAX_COMMITS = 5000

# origin/master 20fa27ac625e, 6290 messages, measured 2026-11-11; 4x headroom.
MAX_COMMIT_MESSAGE_BYTES = 70_469 * 4
MAX_COMMIT_MESSAGE_LINES = 1_459 * 4
# PRs 2906-3395, 300 bodies (corpus sha256 5a4d6f5c3cbf), measured 2026-11-11;
# largest 58,966 bytes (PR 3045) and 544 lines (PR 3041); 4x headroom.
MAX_PR_BODY_BYTES = 58_966 * 4
MAX_PR_BODY_LINES = 544 * 4
# JSON can escape each body byte as six ASCII bytes; allow 1 MiB event metadata.
MAX_PR_EVENT_BYTES = MAX_PR_BODY_BYTES * 6 + 1_048_576
# One commit per read; subject can repeat the message, plus bounded identity metadata.
MAX_COMMIT_LOG_BYTES = MAX_COMMIT_MESSAGE_BYTES * 2 + 65_536

# GitHub logins (as the API reports them) whose commits are exempt.  Keep the
# list in docs/development/dco.md in step with this one.
BOT_LOGINS = ("renovate[bot]", "dependabot[bot]", "github-actions[bot]")

# All 13 tokens equal praetor tools/planning/commit_msg_check.py BANNED_COAUTHORS;
# both Python tuples use substring matching. Engine internal/forge/commit_message.go
# bannedCoauthorsRegex holds only the 11 word tokens, with word-boundary matching;
# the e-mail and [bot] entries are Python-only. Update both tuples together and
# synchronize word-token changes with the engine regex word list.
# On GitHub squash merges, GitHub (noreply@github.com) is the committer on every
# landed commit, so committer identity cannot authorize a commit sign-off; only
# the commit author or the designated landing identity is accepted.
BANNED_COAUTHOR_TOKENS = (
    "claude",
    "anthropic",
    "openai",
    "chatgpt",
    "gpt",
    "codex",
    "gemini",
    "copilot",
    "cursor",
    "opencode",
    "antigravity",
    "noreply@anthropic.com",
    "[bot]",
)

GENERATED_LINE_RE = re.compile(
    r"^(?:generated|created)\s+(?:with|by)\b",
    re.IGNORECASE,
)
ORDERED_MARKER_RE = re.compile(r"[0-9]{1,9}[.)]")
SIGNOFF_RE = re.compile(r"^signed-off-by:\s+(.+?)\s+<([^<>\s]+@[^<>\s]+)>\s*$", re.IGNORECASE)
FIELD_SEP = "\0"
LOG_FIELDS = 7  # sha, author name/e-mail, committer name/e-mail, subject, body
LOG_FORMAT = "%x00".join(["%H", "%an", "%ae", "%cn", "%ce", "%s", "%B"])


class UsageError(Exception):
    """Raised when git cannot answer; the gate must not read that as a pass."""


class InputBoundError(Exception):
    """Attribution input exceeds a named bound; refuse rather than truncate."""


@dataclass(frozen=True)
class Commit:
    sha: str
    author_name: str
    author_email: str
    committer_name: str
    committer_email: str
    subject: str
    message: str


def run_git(
    args: list[str], stdin: str | None = None, max_output_bytes: int = MAX_COMMIT_LOG_BYTES
) -> str:
    """Run bounded Git; failure never becomes an empty successful answer."""
    try:
        done = run(
            [GIT, *args],
            allowed_executables=(GIT,),
            input_data=stdin,
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=GIT_TIMEOUT_S,
            max_output_bytes=max_output_bytes,
        )
    except CommandOutputLimitExceeded as exc:
        raise InputBoundError(
            f"git output exceeds MAX_COMMIT_LOG_BYTES ({max_output_bytes}); "
            f"MAX_COMMIT_MESSAGE_BYTES is {MAX_COMMIT_MESSAGE_BYTES}"
        ) from exc
    except (OSError, CommandTimedOut) as exc:
        raise UsageError(f"git {' '.join(args)}: {exc}") from exc
    if done.returncode != 0:
        raise UsageError(f"git {' '.join(args)} exited {done.returncode}: {done.stderr.strip()}")
    return done.stdout


def parse_commits(out: str) -> list[Commit]:
    """Parse NUL fields and git log -z's NUL record terminator."""
    if not out:
        return []
    fields = out.split(FIELD_SEP)
    if fields.pop() != "" or len(fields) % LOG_FIELDS:
        raise UsageError("unparseable NUL-terminated git log record")
    count = len(fields) // LOG_FIELDS
    if count > MAX_COMMITS:
        raise UsageError(f"{count} commits exceed the bound of {MAX_COMMITS}")
    return [Commit(*fields[i : i + LOG_FIELDS]) for i in range(0, len(fields), LOG_FIELDS)]


def list_commits(base: str, head: str) -> list[Commit]:
    """Read bounded metadata, then each non-merge commit separately, oldest first."""
    out = run_git(
        [
            "log",
            "--no-merges",
            "--reverse",
            f"--max-count={MAX_COMMITS + 1}",
            "--format=%H",
            f"{base}..{head}",
        ]
    )
    shas = out.splitlines()
    if len(shas) > MAX_COMMITS:
        raise UsageError(f"commits exceed the bound of {MAX_COMMITS}")
    commits = []
    for sha in shas:
        commits.extend(list_head_commit(sha))
    return commits


def list_head_commit(head: str) -> list[Commit]:
    """Read one commit, bounding captured bytes before parsing any message lines."""
    out = run_git(["log", "-z", "--no-merges", "-1", f"--format={LOG_FORMAT}", head])
    return parse_commits(out)


def is_ancestor(base: str, head: str) -> bool:
    """Return True if base is an ancestor of head, False if not or not a valid object."""
    try:
        done = run(
            [GIT, "merge-base", "--is-ancestor", base, head],
            allowed_executables=(GIT,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=GIT_TIMEOUT_S,
        )
    except (OSError, CommandTimedOut) as exc:
        raise UsageError(f"git merge-base --is-ancestor: {exc}") from exc
    if done.returncode == 0:
        return True
    if done.returncode in (1, 128):
        return False
    raise UsageError(f"git merge-base exited {done.returncode}: {done.stderr.strip()}")


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


def pr_attribution_reason(
    commit: Commit, author: str, author_type: str, opener: tuple[str, str] | None = None
) -> str | None:
    """Refuse bot authors outside their own branches, then apply shared attribution rules."""
    bot_author = commit.author_name.lower().endswith(
        "[bot]"
    ) or commit.author_email.lower().endswith("[bot]@users.noreply.github.com")
    if opener is not None:
        owns_branch = (commit.author_name, commit.author_email) == opener
    else:
        owns_branch = author_type == "Bot" and commit.author_email.lower().endswith(
            bot_noreply_suffix(author)
        )
    if bot_author and not owns_branch:
        return (
            f"bot author {commit.author_name} <{commit.author_email}> is not the branch opener; "
            "a squash landing would carry it as a Co-authored-by trailer that the landed gate "
            "refuses; re-author that commit with git commit --amend --reset-author --no-edit "
            "or drop it"
        )
    return commit_attribution_reason(commit)


def judge(
    commit: Commit, author: str, author_type: str, opener: tuple[str, str] | None = None
) -> str | None:
    """Return None when the commit passes, else the reason it does not."""
    reason = pr_attribution_reason(commit, author, author_type, opener)
    if reason:
        return reason
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


def check_banned_coauthor(line: str) -> str | None:
    """Return the banned token if the line is a Co-authored-by with a forbidden token."""
    stripped = line.strip()
    if stripped.lower().startswith("co-authored-by:"):
        lower = stripped.lower()
        for token in BANNED_COAUTHOR_TOKENS:
            if token in lower:
                return token
    return None


def ordered_marker_end(line: str, start: int) -> int:
    """Return the end of a terminated ordered marker, or start when none matches."""
    # CommonMark 0.31.2 §5.2, List items: 1-9 ASCII digits followed by '.' or ')'.
    # https://spec.commonmark.org/0.31.2/#list-items
    match = ORDERED_MARKER_RE.match(line, start)
    if match is None:
        return start
    end = match.end()
    if end == len(line) or line[end].isspace() or unicodedata.category(line[end]) == "Cf":
        return end
    return start


def strip_attribution_prefix(line: str) -> str:
    """Remove leading comment, list, quote, heading, whitespace, and format prefixes linearly."""
    start = 0
    # Each iteration consumes at least one character: at most len(line) iterations.
    while start < len(line):
        if line.startswith("<!--", start):
            start += 4
        elif (
            line[start] in "-+*#>"
            or line[start].isspace()
            or unicodedata.category(line[start]) == "Cf"
        ):
            start += 1
        else:
            end = ordered_marker_end(line, start)
            if end == start:
                break
            start = end
    return line[start:]


def check_machine_attribution(line: str) -> bool:
    """Return True if line matches machine attribution patterns or contains robot emoji."""
    return bool(GENERATED_LINE_RE.match(strip_attribution_prefix(line)) or "\U0001f916" in line)


def normalize_attribution_text(text: str) -> str:
    """Normalize compatibility characters for both line and identity matching."""
    return unicodedata.normalize("NFKC", text)


def drop_format_characters(text: str) -> str:
    """Remove every Unicode format character for both line and identity matching."""
    return "".join(character for character in text if unicodedata.category(character) != "Cf")


def input_bound_reason(text: str, source: str, max_bytes: int, max_lines: int) -> str | None:
    """Reject UTF-8 byte/Unicode splitlines bounds before attribution scanning."""
    if len(text.encode("utf-8")) > max_bytes:
        return f"{source} exceeds MAX_{source}_BYTES ({max_bytes})"
    if len(text.splitlines()) > max_lines:
        return f"{source} exceeds MAX_{source}_LINES ({max_lines})"
    return None


def line_attribution_reason(line: str, source: str) -> str | None:
    """Apply shared message and PR body rules without exempting Markdown prefixes."""
    line = strip_attribution_prefix(normalize_attribution_text(line))
    line = drop_format_characters(line)
    token = check_banned_coauthor(line)
    if token:
        return f"disallowed Co-authored-by trailer ({token})"
    if check_machine_attribution(line):
        return f"{source} contains machine-generated attribution"
    return None


def commit_attribution_reason(commit: Commit) -> str | None:
    """Return the first forbidden identity or message attribution for a commit."""
    bound = input_bound_reason(
        commit.message, "COMMIT_MESSAGE", MAX_COMMIT_MESSAGE_BYTES, MAX_COMMIT_MESSAGE_LINES
    )
    if bound:
        return bound
    for role, name, email in (
        ("author", commit.author_name, commit.author_email),
        ("committer", commit.committer_name, commit.committer_email),
    ):
        identity = drop_format_characters(normalize_attribution_text(f"{name} <{email}>")).lower()
        for candidate in BANNED_COAUTHOR_TOKENS:
            if candidate != "[bot]" and candidate in identity:
                return f"{role} identity names a model or coding tool ({candidate})"
    for line in commit.message.splitlines():
        reason = line_attribution_reason(line, "message")
        if reason:
            return reason
    return None


def judge_landed(commit: Commit, landing_email: str) -> str | None:
    """Judge a commit on master: refuse bot co-authors, generated trailers, and bad sign-offs."""
    reason = commit_attribution_reason(commit)
    if reason:
        return reason
    emails = signoff_emails(commit.message)
    if not emails:
        return "no 'Signed-off-by: Name <email>' trailer"
    allowed = {commit.author_email.lower(), landing_email.lower()}
    if allowed.isdisjoint(emails):
        return f"sign-off ({', '.join(emails)}) matches neither author nor landing identity"
    return None


def extract_email(identity: str) -> str:
    """Extract email address from an identity string like 'Name <email>' or 'email'."""
    match = re.search(r"<([^<>\s]+@[^<>\s]+)>", identity)
    if match:
        return match.group(1).lower()
    return identity.strip().lower()


def is_signoff_reason(reason: str) -> bool:
    """Distinguish sign-off failures from attribution failures for repair advice."""
    return reason.startswith(("no 'Signed-off-by:", "sign-off ("))


def report(failures: list[tuple[Commit, str]], landed: bool = False) -> None:
    """Print commit failures and repair advice appropriate to sign-off or attribution errors."""
    for commit, reason in failures:
        title = "DCO sign-off" if is_signoff_reason(reason) else "DCO attribution"
        print(f"::error title={title}::{commit.sha[:10]} {commit.subject}: {reason}")
    print("")
    signoff_failed = any(is_signoff_reason(reason) for _, reason in failures)
    attribution_failed = any(not is_signoff_reason(reason) for _, reason in failures)
    if landed:
        print(f"dco: {len(failures)} landed commit(s) refused.")
        if signoff_failed:
            print("  Landed commit check (ADR-3389): squash merge requires an explicit body file")
            print("  (--body-file / -F) containing commit body and sign-off.")
        print("  Alarm: commit already on master; published history is not rewritten.")
    else:
        print(f"dco: {len(failures)} commit(s) refused.")
        if signoff_failed:
            print("  Add the sign-off to each affected commit, then push again:")
            print("    git rebase --signoff $(git merge-base origin/master HEAD)")
            print("  New commits: git commit -s   (keep the trailers when squashing)")
    if attribution_failed and landed:
        print("  The squash body or author identity carried model/tool attribution;")
        print("  fix the landing tool's body file and author before the next merge.")
    elif attribution_failed:
        print("  Remove model/tool trailers or generated footers; re-author affected commits")
        print("  as yourself, and edit the pull request body to remove attribution.")
    if signoff_failed:
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


def read_bounded_file(path: Path, max_bytes: int, bound: str) -> str:
    """Read at most bound+1 bytes; oversized files fail without truncation."""
    try:
        with path.open("rb") as stream:
            data = stream.read(max_bytes + 1)
        if len(data) > max_bytes:
            raise InputBoundError(f"{path}: exceeds {bound} ({max_bytes})")
        return data.decode("utf-8")
    except (OSError, UnicodeError) as exc:
        raise UsageError(f"PR body file/event {path}: {exc}") from exc


def read_event_body() -> str:
    """Read the pull-request body from the file identified by GITHUB_EVENT_PATH."""
    path = os.environ.get("GITHUB_EVENT_PATH", "")
    if not path:
        raise UsageError("PR body event: GITHUB_EVENT_PATH is required")
    text = read_bounded_file(Path(path), MAX_PR_EVENT_BYTES, "MAX_PR_EVENT_BYTES")
    try:
        payload = json.loads(text)
        body = payload["pull_request"]["body"]
    except (ValueError, KeyError, TypeError) as exc:
        raise UsageError(f"PR body event {path}: invalid pull_request.body") from exc
    if body is not None and not isinstance(body, str):
        raise UsageError(f"PR body event {path}: body must be a string or null")
    return body or ""


def read_pr_body(args: argparse.Namespace) -> str:
    """Explicit body file or event file, otherwise the local environment fallback."""
    if args.pr_body_file:
        return read_bounded_file(Path(args.pr_body_file), MAX_PR_BODY_BYTES, "MAX_PR_BODY_BYTES")
    if args.pr_body_from_event:
        return read_event_body()
    return str(args.pr_body)


def pr_body_attribution(body: str) -> list[tuple[int, str]]:
    """Return forbidden PR body lines with one-based line numbers."""
    bound = input_bound_reason(body, "PR_BODY", MAX_PR_BODY_BYTES, MAX_PR_BODY_LINES)
    if bound:
        return [(0, bound)]
    findings = []
    for number, line in enumerate(body.splitlines(), start=1):
        reason = line_attribution_reason(line, "PR body")
        if reason:
            findings.append((number, reason))
    return findings


def check_landed(args: argparse.Namespace) -> int:
    head, base = args.head, args.base
    run_git(["rev-parse", "--verify", f"{head}^{{commit}}"])
    is_zero_base = bool(re.fullmatch(r"0{40}", base))
    if is_zero_base or not is_ancestor(base, head):
        print(
            f"dco: base ({base[:10]}) is all-zero or not ancestor-reachable; judging head commit only."
        )
        commits = list_head_commit(head)
    else:
        commits = list_commits(base, head)
    landing_email = extract_email(args.landing_identity)
    failures = []
    for commit in commits:
        reason = judge_landed(commit, landing_email)
        if reason is not None:
            failures.append((commit, reason))
    if failures:
        report(failures, landed=True)
        return 1
    print(f"dco: PASS - {len(commits)} commit(s) verified on master.")
    return 0


def pr_signoff_exemption(args: argparse.Namespace) -> str | None:
    """Return the sign-off exemption notice when release or cutoff rules apply."""
    if args.release_pr:
        return "dco: machine-generated release pull request, exempt."
    if grandfathered(args.created_at, Path(args.cutoff_file)):
        return f"dco: pull request created {args.created_at}, before the cutoff; grandfathered."
    return None


def branch_opener(commits: list[Commit], pre_push: bool) -> tuple[str, str] | None:
    """Infer pre-push ownership from the oldest non-merge commit, if one exists."""
    if pre_push and commits:
        return commits[0].author_name, commits[0].author_email
    return None


def check_pr(args: argparse.Namespace) -> int:
    """Enforce commit and body attribution on every PR, and commit sign-offs unless exempt."""
    commits = list_commits(args.base, args.head)
    opener = branch_opener(commits, bool(args.merge_base))
    exemption = pr_signoff_exemption(args)
    judge_commit: Callable[[Commit], str | None]
    if exemption is not None:
        print(exemption)
        judge_commit = functools.partial(
            pr_attribution_reason, author=args.author, author_type=args.author_type, opener=opener
        )
        kind = "release pull request" if args.release_pr else "grandfathered"
        summary = f" free of attribution; sign-off exempt ({kind})"
    else:
        judge_commit = functools.partial(
            judge, author=args.author, author_type=args.author_type, opener=opener
        )
        summary = ", every one signed off or exempt"
    body_findings = pr_body_attribution(read_pr_body(args))
    failures = [(c, reason) for c in commits if (reason := judge_commit(c)) is not None]
    if failures:
        report(failures)
    for number, reason in body_findings:
        print(f"::error title=PR body attribution::line {number}: {reason}")
    if body_findings:
        print("  Edit the pull request body to remove model/tool attribution.")
    if failures or body_findings:
        return 1
    print(f"dco: PASS - {len(commits)} commit(s){summary}.")
    return 0


def resolve_push_range(args: argparse.Namespace) -> None:
    """Point ``--base`` at REF and ``--head`` at the outgoing commit.

    ``REF..head`` is the merge-base range without its criss-cross caveat: it
    leaves out every commit reachable from REF, including those a branch took
    in by merging REF.
    """
    if args.base:
        raise UsageError("--merge-base and --base (or BASE_SHA) are mutually exclusive")
    if args.landed:
        raise UsageError("--merge-base checks a branch before push; it cannot take --landed")
    head = args.head or os.environ.get("PRE_COMMIT_TO_REF", "") or "HEAD"
    args.head = run_git(["rev-parse", "--verify", "--end-of-options", f"{head}^{{commit}}"])
    args.head = args.head.strip()
    base = run_git(["rev-parse", "--verify", "--end-of-options", f"{args.merge_base}^{{commit}}"])
    args.base = base.strip()


def check(args: argparse.Namespace) -> int:
    if args.merge_base:
        resolve_push_range(args)
    if args.landed:
        return check_landed(args)
    return check_pr(args)


def parse_args(argv: list[str]) -> argparse.Namespace:
    """Parse revision, identity, body-source, and enforcement options with environment defaults."""
    env = os.environ.get
    parser = argparse.ArgumentParser(description=__doc__.split("\n", 1)[0])
    parser.add_argument("--base", default=env("BASE_SHA", ""))
    parser.add_argument("--head", default=None)
    parser.add_argument("--author", default=env("PR_AUTHOR", ""))
    parser.add_argument("--author-type", default=env("PR_AUTHOR_TYPE", ""))
    parser.add_argument("--created-at", default=env("PR_CREATED_AT", ""))
    parser.add_argument("--cutoff-file", default=str(DEFAULT_CUTOFF_FILE))
    body = parser.add_mutually_exclusive_group()
    body.add_argument("--pr-body-file", "--pr-body", default="")
    body.add_argument("--pr-body-from-event", action="store_true")
    parser.set_defaults(pr_body=env("PR_BODY", ""))
    parser.add_argument(
        "--release-pr", action="store_true", default=env("DCO_RELEASE_PR", "") == "true"
    )
    parser.add_argument("--landed", action="store_true")
    parser.add_argument("--merge-base", default="", metavar="REF")
    parser.add_argument("--landing-identity", default=env("LANDING_IDENTITY", ""))
    args = parser.parse_args(argv)
    if args.head is None:
        # --merge-base names the pushed commit itself; HEAD_SHA must not replace it.
        args.head = "" if args.merge_base else env("HEAD_SHA", "")
    return args


def main(argv: list[str]) -> int:
    """Validate required inputs and return distinct statuses for policy and usage failures."""
    args = parse_args(argv)
    if not args.merge_base and (not args.base or not args.head):
        print(
            "check-dco: --base and --head (or BASE_SHA and HEAD_SHA), or --merge-base,"
            " are required",
            file=sys.stderr,
        )
        return 2
    if args.landed and not args.landing_identity:
        print(
            "check-dco: --landing-identity (or LANDING_IDENTITY) is required with --landed",
            file=sys.stderr,
        )
        return 2
    try:
        return check(args)
    except InputBoundError as exc:
        print(f"check-dco: {exc}", file=sys.stderr)
        return 1
    except UsageError as exc:
        print(f"check-dco: {exc}", file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
