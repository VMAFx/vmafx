#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse new Codacy Bandit findings in changed Python files."""

from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.new_scanner_findings import (
    ToolError,
    base_text,
    merge_base,
    new_findings,
    renames,
    tool,
)
from scripts.lib.safe_subprocess import run as run_command

CONFIG = Path(__file__).resolve().parents[2] / "bandit.yml"


def findings(bandit: str, source: Path) -> list[tuple[int, str, str]]:
    """Read Bandit JSON and return rule IDs keyed by stripped source lines."""
    try:
        proc = run_command(
            [bandit, "-c", str(CONFIG), "-f", "json", str(source)],
            allowed_executables=(bandit,),
            capture_output=True,
            text=True,
            timeout_seconds=120,
            max_output_bytes=8 * 1_048_576,
        )
        if proc.returncode not in (0, 1):
            raise ToolError(f"bandit exited {proc.returncode} on {source}: {proc.stderr.strip()}")
        report = json.loads(proc.stdout)
        if not isinstance(report, dict) or not isinstance(report.get("results"), list):
            raise ToolError(f"invalid Bandit JSON on {source}")
        if not isinstance(report.get("errors"), list) or report["errors"]:
            raise ToolError(f"Bandit reported scan errors on {source}: {report.get('errors')}")
        lines = source.read_text(encoding="utf-8").splitlines()
        found = []
        for issue in report["results"]:
            number, rule = issue["line_number"], issue["test_id"]
            if not isinstance(number, int) or number < 1 or number > len(lines):
                raise ToolError(f"invalid Bandit line on {source}: {number}")
            if not isinstance(rule, str):
                raise ToolError(f"invalid Bandit rule on {source}")
            found.append((number, rule, lines[number - 1].strip()))
        return found
    except (OSError, ValueError, KeyError, TypeError, RuntimeError) as error:
        raise ToolError(f"Bandit failed on {source}: {error}") from error


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").strip())
    parser.add_argument("--base-branch", default="origin/master")
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)
    root = Path.cwd()
    try:
        bandit, git = tool("bandit"), tool("git")
        if not CONFIG.is_file():
            raise ToolError(f"Bandit configuration missing: {CONFIG}")
        base = merge_base(git, root, args.base_branch)
        moved = renames(git, root, base)
        failed = 0
        for path in args.files:
            if path.suffix != ".py" or not path.is_file():
                continue
            relative = path.resolve().relative_to(root.resolve()).as_posix()
            old = base_text(git, root, base, moved.get(relative, relative))
            for line, rule, source_line in new_findings(
                lambda source: findings(bandit, source), path, old
            ):
                print(f"{relative}:{line}: {rule}: {source_line}")
                failed += 1
    except (ToolError, OSError, ValueError, RuntimeError) as error:
        print(f"check_bandit_new: {error}", file=sys.stderr)
        return 2
    if failed:
        print(
            f"{failed} new Bandit finding(s); run processes through "
            "scripts/lib/safe_subprocess.run",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
