#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Reject retired dual copyright rules in agent guidance (ADR-0861).

ADR-0861 supersedes ADR-0025's line format: wholly-new fork files carry
``Copyright 2026 Lusoris``; upstream-derived files retain their headers.
Scan tracked agent rules, CONTRIBUTING.md and development documentation.
Exit 0 on clean rules, 1 on retired wording, 2 on Git or file-read errors.
"""

from __future__ import annotations

import argparse
import os
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

from scripts.lib import git_env  # noqa: E402
from scripts.lib.safe_subprocess import CommandOutputLimitExceeded  # noqa: E402
from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

PATHSPECS = [
    "AGENTS.md",
    "*/AGENTS.md",
    "*/AGENTS.d/*.md",
    "CONTRIBUTING.md",
    "docs/development/*.md",
]
RETIRED_RULE = re.compile(r"Lusoris\s*[/+]\s*Claude|dual\s+Lusoris", re.IGNORECASE)


def git_environment(root: Path) -> dict[str, str]:
    """Return the environment git runs in for `root`.

    In this checkout a commit hook's GIT_INDEX_FILE names the staged state, which is what to
    check. For any other tree (a test fixture) the caller's GIT_* variables would point git at
    the caller's repository, so they are dropped. Same semantics as
    check_licence_metadata.git_environment; one shared helper is pending (issue #3324).
    """
    if root.resolve() == ROOT.resolve():
        return dict(os.environ)
    return git_env.drop()


def tracked_rules(root: Path) -> list[str]:
    """Return the bounded Git index listing for the explicit rule pathspecs."""
    result = run_command(
        ["git", "ls-files", "-z", "--", *PATHSPECS],
        allowed_executables=("git",),
        cwd=root,
        env=git_environment(root),
        capture_output=True,
        text=True,
        encoding="utf-8",
        timeout_seconds=30,
    )
    if result.returncode:
        raise OSError(f"git ls-files exited {result.returncode}: {result.stderr.strip()}")
    return [name for name in result.stdout.split("\0") if name]


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=ROOT, help="repository root")
    args = parser.parse_args(argv)
    findings: list[str] = []
    try:
        names = tracked_rules(args.root)
        for name in names:
            lines = (args.root / name).read_text(encoding="utf-8").splitlines()
            for number, line in enumerate(lines, start=1):
                if RETIRED_RULE.search(line):
                    findings.append(f"{name}:{number}: {line.strip()}")
    except (OSError, UnicodeError, ValueError, CommandOutputLimitExceeded) as exc:
        print(f"check_agent_copyright_rule: cannot check {args.root}: {exc}", file=sys.stderr)
        return 2
    for finding in findings:
        print(finding, file=sys.stderr)
    if findings:
        return 1
    print(f"agent copyright rules: {len(names)} tracked files checked; ADR-0861 holds")
    return 0


if __name__ == "__main__":
    sys.exit(main())
