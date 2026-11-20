#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reject obsolete GitHub organisation references in tracked text.

Exit: 0 clean, 1 finding, 2 usage error.
"""

from __future__ import annotations

import argparse
import datetime as dt
import fnmatch
import os
import re
import shutil
import sys
from pathlib import Path
from typing import NamedTuple

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import (
    CommandFailed,
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
)
from scripts.lib.safe_subprocess import run as run_command

_GIT_PATH = shutil.which("git")
GIT = str(Path(_GIT_PATH).resolve(strict=True)) if _GIT_PATH is not None else None
MAX_GIT_OUTPUT_BYTES = 16 * 1_048_576


class ExceptionRule(NamedTuple):
    paths: tuple[str, ...]
    reason: str
    line_pattern: re.Pattern[str] | None = None
    prefix_pattern: re.Pattern[str] | None = None


PERMANENT = (
    "docs/adr/[0-9][0-9][0-9][0-9]-*.md",
    "docs/adr/_index_fragments/**",
    "docs/research/**",
    "scripts/ci/research-digest-id-baseline.json",
    "docs/rebase-notes.d/**",
    "docs/rebase-notes.md",
    "changelog.d/**",
    "CHANGELOG.md",
    "docs/changelog-archive/**",
    "scripts/ci/tests/test_check_old_org_refs.py",
)
EXPIRY = dt.date(2026, 11, 30)
ENTRY = dt.date(2026, 11, 20)
EXCEPTIONS = (
    ExceptionRule(
        (
            "core/include/libvmaf/pelorus/*.h",
            "core/src/interop/pelorus_*.c",
            "core/test/test_pelorus_interop.c",
            "docs/api/pelorus-interop.md",
        ),
        "vendored mirror; banner changes when Pelorus is re-vendored from vidiko/pelorus",
    ),
    ExceptionRule(
        ("build-config.env",),
        "ffmpeg-patches v0.7.0 is signed under the VMAFx workflow identity; the pin moves with the next series release",
        line_pattern=re.compile(r"^\s*(?:export\s+)?FFMPEG_FIX_SERIES_REPO\s*="),
    ),
    ExceptionRule(
        (
            "lua/standards.lua",
            "tools/markdownlint/package.json",
            "tools/markdownlint/package-lock.json",
        ),
        "rendered by Praetor; follows when Praetor renames them",
    ),
    ExceptionRule(
        ("**",),
        "k8s and chart image references switch in the follow-up PR once vidiko image tags exist",
        prefix_pattern=re.compile(r"ghcr\.io/$", re.I),
    ),
)
ORG = r"(?:vmafx|cordanallm|golusoris)"
PATTERNS = (
    ("d", re.compile(r"https?://github\.com/" + ORG + r'/[^\s`"\'<>)]*', re.I)),
    ("e", re.compile(r"\b" + ORG + r"\.github\.io\b", re.I)),
    ("f", re.compile(r"github\.com/orgs/" + ORG + r'/[^\s`"\'<>)]*', re.I)),
    (
        "g",
        re.compile(
            r"""(?:["']?owner["']?\s*[:=]\s*["']?|--owner\s+["']?)"""
            + r"(?:VMAFx|cordanaLLM|cordanallm|golusoris)"
            + r"\b"
        ),
    ),
    ("a", re.compile(r"(?<!github\.com/)\bVMAFx/[A-Za-z0-9_.-]+")),
    ("b", re.compile(r"(?<!github\.com/)\b(?:cordanaLLM|cordanallm)/[A-Za-z0-9_.-]+")),
    ("c", re.compile(r"(?<!github\.com/)\bgolusoris/golusoris\b")),
)


def allowed(path: str) -> bool:
    return any(fnmatch.fnmatchcase(path, pattern) for pattern in PERMANENT)


def exception(path: str, line: str, start: int) -> str | None:
    for rule in EXCEPTIONS:
        if not any(fnmatch.fnmatchcase(path, pattern) for pattern in rule.paths):
            continue
        if rule.line_pattern and not rule.line_pattern.match(line):
            continue
        if rule.prefix_pattern and not rule.prefix_pattern.search(line[:start]):
            continue
        return rule.reason
    return None


def line_findings(path: str, number: int, line: str, today: dt.date) -> list[str]:
    if "old-org-ok" in line:
        return []
    hits: list[str] = []
    occupied: list[tuple[int, int]] = []
    for letter, pattern in PATTERNS:
        for match in pattern.finditer(line):
            if any(match.start() < end and match.end() > start for start, end in occupied):
                continue
            occupied.append(match.span())
            reason = exception(path, line, match.start())
            if reason and today <= EXPIRY:
                continue
            suffix = f" [expired exception: {reason}; MEL C, {ENTRY} to {EXPIRY}]" if reason else ""
            hits.append(
                f"{path}:{number}: old organisation reference ({letter}): {match.group()}{suffix}"
            )
    return hits


def scan(path: str, today: dt.date) -> list[str]:
    if allowed(path) or Path(path).is_dir():
        return []
    try:
        data = Path(path).read_bytes()
        if b"\0" in data[:8192]:
            return []
        content = data.decode("utf-8")
    except UnicodeDecodeError:
        return []
    findings = []
    for number, line in enumerate(content.splitlines(), 1):
        findings.extend(line_findings(path, number, line, today))
    return findings


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--all", action="store_true")
    parser.add_argument("paths", nargs="*")
    args = parser.parse_args()
    if args.all == bool(args.paths):
        parser.error("provide --all or one or more paths")
    try:
        today = dt.date.fromisoformat(os.environ.get("OLD_ORG_REFS_TODAY", str(dt.date.today())))
        paths = args.paths
        if args.all:
            if GIT is None:
                raise ValueError("required executable not found: git")
            result = run_command(
                [GIT, "ls-files", "-z"],
                allowed_executables=(GIT,),
                env=None,
                capture_output=True,
                check=True,
                timeout_seconds=60,
                max_output_bytes=MAX_GIT_OUTPUT_BYTES,
            )
            paths = [os.fsdecode(p) for p in result.stdout.split(b"\0") if p]
        findings = [hit for path in paths for hit in scan(path, today)]
    except (
        ValueError,
        OSError,
        CommandFailed,
        CommandTimedOut,
        CommandOutputLimitExceeded,
        CommandValidationError,
    ) as error:
        print(f"check-old-org-refs: {' '.join(str(error).splitlines())}", file=sys.stderr)
        return 2
    for hit in findings:
        print(hit)
    print(f"check-old-org-refs: {len(findings)} finding(s)", file=sys.stderr)
    return int(bool(findings))


if __name__ == "__main__":
    sys.exit(main())
