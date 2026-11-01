#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Fork Python under python/ and testdata/ passes the process and file-mode rules.

pyproject.toml exempts python/** and testdata/** from ruff's S family for the
upstream Netflix harness. That exemption also hid fork-written files there, and
Codacy's Bandit then flagged them on the pull request (B603, B607, B103). This
check runs exactly those three rules on the given files whose licence header names
the fork licence, EUPL-1.2, without the per-file exemption.
Run a process through scripts/lib/safe_subprocess.run instead.

Usage: check_fork_python_subprocess.py FILE...; exit 1 lists each finding.
"""

from __future__ import annotations

import shutil
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
if str(REPO) not in sys.path:
    sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import run  # noqa: E402 - needs the repository root

RULES = "S103,S603,S607"
SCOPES = ("python/", "testdata/")
HEADER_LINES = 5
FORK_SPDX = "SPDX-License-" + "Identifier: EUPL-1.2"  # split: REUSE reads the literal


def fork_files(paths: list[str]) -> list[str]:
    """The paths under the exempt scopes whose header names the fork licence."""
    chosen: list[str] = []
    for name in paths:
        rel = Path(name).as_posix()
        if not (rel.endswith(".py") and rel.startswith(SCOPES)) or not Path(name).is_file():
            continue
        with Path(name).open(encoding="utf-8", errors="replace") as handle:
            head = [handle.readline() for _ in range(HEADER_LINES)]
        if any(FORK_SPDX in line for line in head):
            chosen.append(name)
    return chosen


def main(argv: list[str]) -> int:
    files = fork_files(argv)
    if not files:
        return 0
    ruff = shutil.which("ruff")
    if ruff is None:
        print("check_fork_python_subprocess: ruff is not on PATH", file=sys.stderr)
        return 2
    result = run(
        [
            ruff,
            "check",
            "--isolated",
            "--no-cache",
            "--select",
            RULES,
            "--output-format",
            "concise",
            *files,
        ],
        allowed_executables=[ruff],
        capture_output=True,
        text=True,
        timeout_seconds=120,
    )
    if result.returncode == 0:
        return 0
    sys.stderr.write(result.stdout + result.stderr)
    print(
        "check_fork_python_subprocess: run processes through scripts/lib/safe_subprocess.run "
        "and give files owner-only modes",
        file=sys.stderr,
    )
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
