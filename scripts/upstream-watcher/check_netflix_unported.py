#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Check for unported Netflix/vmaf master commits after the parity pin.

Reads the parity pin from docs/development/known-upstream-bugs.md, lists commits
between the pin and the upstream reference, filters out commits recorded in the
dispositions ledger, and prints any remaining unported commits.

Exit status:
  0: at least one unported commit found
  1: no unported commits found (all commits recorded)
  2: infrastructure or record error (missing ref, bad ledger, pin not ancestor)
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path
from typing import NamedTuple

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.ci.upstream_parity_pin import (  # noqa: E402
    RECORD,
    GitRunner,
    PinError,
    git_runner,
    recorded_pin,
    resolve,
)

VALID_STATES = frozenset({"ported", "in-fork", "not-applicable"})
SHA_PATTERN = re.compile(r"^[0-9a-f]{40}$")
PR_SUFFIX_PATTERN = re.compile(r"\s+\(#\d+\)$")
LEDGER_COLUMN_COUNT = 3


class LedgerError(ValueError):
    """The dispositions ledger is malformed, missing, or invalid."""


class CommitInfo(NamedTuple):
    sha: str
    subject: str


def load_ledger(path: Path) -> dict[str, tuple[str, str]]:
    """Parse *path*, returning a mapping from commit sha to (state, reference).

    Refuses duplicate sha, invalid sha format, unknown state, or empty reference.
    """
    if not path.is_file():
        raise LedgerError(f"ledger file not found: {path}")
    try:
        content = path.read_text(encoding="utf-8")
    except OSError as err:
        raise LedgerError(f"cannot read ledger file {path}: {err}") from err

    entries: dict[str, tuple[str, str]] = {}
    for line_no, raw_line in enumerate(content.splitlines(), start=1):
        line = raw_line.strip()
        if not line or line.startswith("#"):
            continue
        parts = raw_line.split("\t")
        if len(parts) != LEDGER_COLUMN_COUNT:
            raise LedgerError(
                f"malformed line {line_no} in {path}: expected {LEDGER_COLUMN_COUNT} "
                f"tab-separated columns, got {len(parts)}: {raw_line!r}"
            )
        sha, state, reference = parts[0].strip(), parts[1].strip(), parts[2].strip()
        if not SHA_PATTERN.match(sha):
            raise LedgerError(
                f"invalid commit sha at line {line_no} in {path}: {sha!r} "
                "(expected 40 lowercase hex digits)"
            )
        if sha in entries:
            raise LedgerError(f"duplicate commit sha at line {line_no} in {path}: {sha}")
        if state not in VALID_STATES:
            raise LedgerError(
                f"unknown state at line {line_no} in {path}: {state!r} "
                f"(expected one of {sorted(VALID_STATES)})"
            )
        if not reference:
            raise LedgerError(f"empty reference at line {line_no} in {path}")
        entries[sha] = (state, reference)

    return entries


def clean_subject(raw_subject: str) -> str:
    """Strip any trailing ` (#NNNN)` PR reference from *raw_subject*."""
    return PR_SUFFIX_PATTERN.sub("", raw_subject.strip())


def get_commit_subject(runner: GitRunner, sha: str) -> str:
    """Fetch the commit subject line for *sha* via git log."""
    status, subject = runner(["log", "-1", "--format=%s", sha])
    if status != 0:
        raise PinError(f"failed to get commit subject for {sha}: {subject}")
    return clean_subject(subject)


def list_unported_commits(
    repo: Path,
    upstream_ref: str,
    ledger_path: Path,
) -> list[CommitInfo]:
    """Identify unported upstream commits in *repo* between pin and *upstream_ref*."""
    record_file = repo / RECORD
    if not record_file.is_file():
        raise PinError(f"record file not found: {record_file}")
    try:
        record_text = record_file.read_text(encoding="utf-8")
    except OSError as err:
        raise PinError(f"cannot read record file {record_file}: {err}") from err

    pin = recorded_pin(record_text)
    runner = git_runner(repo)

    status, _ = runner(["rev-parse", "--verify", "--quiet", f"{upstream_ref}^{{commit}}"])
    if status != 0:
        raise PinError(f"upstream reference '{upstream_ref}' not found in {repo}")

    full_pin = resolve(pin, upstream_ref, runner)
    ledger = load_ledger(ledger_path)

    status, output = runner(["rev-list", "--no-merges", "--reverse", f"{full_pin}..{upstream_ref}"])
    if status != 0:
        raise PinError(f"git rev-list failed: {output}")

    commit_shas = [line.strip() for line in output.splitlines() if line.strip()]
    unported = [sha for sha in commit_shas if sha not in ledger]

    return [CommitInfo(sha=s, subject=get_commit_subject(runner, s)) for s in unported]


def print_results(commits: list[CommitInfo], as_json: bool) -> None:
    """Print *commits* in JSON format or as plain space-separated lines."""
    if as_json:
        payload = [{"sha": c.sha, "subject": c.subject} for c in commits]
        print(json.dumps(payload, indent=2))
    else:
        for c in commits:
            print(f"{c.sha} {c.subject}")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n", 1)[0])
    parser.add_argument(
        "--repo",
        type=Path,
        default=REPO,
        help="path to git checkout with upstream ref (default: repository root)",
    )
    parser.add_argument(
        "--upstream-ref",
        default="upstream/master",
        help="upstream ref to check against (default: upstream/master)",
    )
    parser.add_argument(
        "--ledger",
        type=Path,
        default=None,
        help="path to dispositions ledger tsv (default: scripts/upstream-watcher/netflix-dispositions.tsv)",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="output list of {sha, subject} in JSON format",
    )
    args = parser.parse_args(argv)

    ledger_path = args.ledger
    if ledger_path is None:
        ledger_path = args.repo / "scripts" / "upstream-watcher" / "netflix-dispositions.tsv"

    try:
        unported = list_unported_commits(args.repo, args.upstream_ref, ledger_path)
    except (PinError, LedgerError, OSError, ValueError) as err:
        print(f"check_netflix_unported: {err}", file=sys.stderr)
        return 2

    if not unported:
        if args.json:
            print("[]")
        return 1

    print_results(unported, args.json)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
