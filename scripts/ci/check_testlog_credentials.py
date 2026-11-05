#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Refuse a checkout whose Meson testlog files hold credential-shaped values (ADR-1333).

Meson's test runner writes its parent environment to ``meson-logs/testlog.txt``
and each test's environment to ``testlog.json``. The repository's test
wrapper for Meson removes credentials before Meson starts, but a direct Meson run skips it,
and the logs then sit in the gitignored build directory, where a hosted agent lane
or an archive can read them. This check walks the checkout for those logs and fails
when any line holds a token-shaped value. It prints paths and counts only, never a
value.

Usage: python3 scripts/ci/check_testlog_credentials.py [ROOT ...]
"""

from __future__ import annotations

import os
import re
import sys
from collections.abc import Iterator, Sequence
from pathlib import Path

# Value shapes, not names: GitHub classic and fine-grained tokens, OpenAI-style
# ``sk-`` keys (the gateway key shape) and AWS access key ids.
CREDENTIAL_VALUE_PATTERN = re.compile(
    r"\bgh[pousr]_[A-Za-z0-9]{36}\b"
    r"|\bgithub_pat_[A-Za-z0-9_]{22,}"
    r"|\bsk-[A-Za-z0-9_-]{20,}"
    r"|\bAKIA[0-9A-Z]{16}\b"
)
LOG_NAME_PATTERN = re.compile(r"^testlog.*\.(?:txt|json)$")
SKIP_DIRECTORIES = frozenset({".git", "node_modules", ".venv", "__pycache__"})
MAX_DIRECTORIES = 200_000
MAX_LOG_BYTES = 512 * 1024 * 1024


def _raise_walk_error(error: OSError) -> None:
    """Make ``os.walk`` fail on an unreadable directory instead of skipping it."""
    raise error


def _checked_log(path: Path) -> Path:
    """Return ``path`` when it is a regular file; a symlink or other type is an error."""
    if path.is_symlink() or not path.is_file():
        raise RuntimeError(f"{path} is not a regular file")
    return path


def _test_logs(root: Path) -> Iterator[Path]:
    """Yield every ``meson-logs/testlog*`` file below ``root``, bounded."""
    if not root.is_dir():
        raise RuntimeError(f"{root} is not a directory")
    visited = 0
    for directory, subdirectories, files in os.walk(root, onerror=_raise_walk_error):
        visited += 1
        if visited > MAX_DIRECTORIES:
            raise RuntimeError(f"more than {MAX_DIRECTORIES} directories below {root}")
        subdirectories[:] = [name for name in subdirectories if name not in SKIP_DIRECTORIES]
        if Path(directory).name != "meson-logs":
            continue
        for name in sorted(files):
            if LOG_NAME_PATTERN.match(name):
                yield _checked_log(Path(directory) / name)


def count_credential_lines(path: Path) -> int:
    """Return how many lines of ``path`` hold a credential-shaped value."""
    if path.stat().st_size > MAX_LOG_BYTES:
        raise RuntimeError(f"{path} exceeds {MAX_LOG_BYTES} bytes")
    with path.open(encoding="utf-8", errors="replace") as handle:
        return sum(1 for line in handle if CREDENTIAL_VALUE_PATTERN.search(line))


def findings(roots: Sequence[Path]) -> list[tuple[Path, int]]:
    """Return ``(log path, matching line count)`` for every log with a match."""
    result: list[tuple[Path, int]] = []
    for root in roots:
        for log in _test_logs(root):
            count = count_credential_lines(log)
            if count:
                result.append((log, count))
    return result


def main(argv: Sequence[str] | None = None) -> int:
    """Report logs that hold credential-shaped values; exit 1 when any exists."""
    arguments = list(sys.argv[1:] if argv is None else argv)
    roots = [Path(item) for item in arguments] or [Path.cwd()]
    try:
        found = findings(roots)
    except (OSError, RuntimeError) as exc:
        print(f"check_testlog_credentials.py: {exc}", file=sys.stderr)
        return 2
    for log, count in found:
        print(f"{log.as_posix()}: {count} line(s) hold a credential-shaped value")
    if found:
        print(
            "Delete these meson-logs directories, and run tests through the "
            "repository's test wrapper for Meson, which removes credentials first (ADR-1333).",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
