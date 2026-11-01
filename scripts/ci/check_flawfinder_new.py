#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Refuse flawfinder findings that a change adds to C and C++ sources.

Codacy runs flawfinder on every pull request and fails the pull request on a
new finding (two `fopen()` calls in a Go test harness, #3216). Nothing ran
flawfinder before the push, so the finding first showed up after a full CI
round. This check runs the same engine on the given files and compares each
file with its copy at the merge base with the default branch:

- a finding whose rule name and source line also occur at the base is old and
  passes, wherever the line moved;
- any other finding is new and fails the check.

flawfinder matches function names, not data flow: `open()` and `fopen()` are
reported whatever their flags. A harness reads inputs from descriptors its
caller opened (`fdopen()`, as pkg/libvmaf does, ADR-1526) instead.

Exit 0 when no file gains a finding, 1 on a new finding, 2 when flawfinder or
git is missing or fails.
"""

from __future__ import annotations

import argparse
import collections
import csv
import io
import os
import shutil
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

SUFFIXES = frozenset({".c", ".h", ".cc", ".cpp", ".cxx", ".hpp", ".cu", ".mm"})
Key = tuple[str, str]


class ToolError(RuntimeError):
    """flawfinder or git is missing or failed."""


def tool(name: str) -> str:
    path = shutil.which(name)
    if path is None:
        raise ToolError(f"{name} is not on PATH")
    return path


def findings(flawfinder: str, source: Path) -> list[tuple[int, str, str]]:
    """Return (line, rule name, source line) for each finding in `source`."""
    proc = run_command(
        [flawfinder, "--csv", "--quiet", "--dataonly", str(source)],
        allowed_executables=(flawfinder,),
        capture_output=True,
        text=True,
        timeout_seconds=120,
        max_output_bytes=8 * 1_048_576,
    )
    if proc.returncode != 0:
        raise ToolError(f"flawfinder failed on {source}: {proc.stderr.strip()}")
    rows = csv.DictReader(io.StringIO(proc.stdout))
    return [(int(row["Line"]), row["Name"], row["Context"].strip()) for row in rows]


def git_output(git: str, args: list[str], cwd: Path) -> tuple[int, str]:
    # Read-only git in `cwd`; the hook's GIT_DIR / GIT_INDEX_FILE are dropped so
    # `cwd` alone names the repository.
    env = {k: v for k, v in os.environ.items() if not k.startswith("GIT_")}
    proc = run_command(
        [git, *args],
        allowed_executables=(git,),
        cwd=cwd,
        env=env,
        capture_output=True,
        text=True,
        timeout_seconds=60,
        max_output_bytes=64 * 1_048_576,
    )
    return proc.returncode, proc.stdout


def base_text(git: str, root: Path, base: str, relative: str) -> str:
    """Return the file at `base`, or "" when it did not exist there."""
    status, text = git_output(git, ["show", f"{base}:{relative}"], root)
    return text if status == 0 else ""


def new_findings(flawfinder: str, current: Path, old_text: str) -> list[tuple[int, str, str]]:
    """Return the findings of `current` that its old text does not carry."""
    with tempfile.TemporaryDirectory(prefix="flawfinder-base-") as temp:
        old = Path(temp) / current.name
        old.write_text(old_text, encoding="utf-8")
        old_keys = collections.Counter((name, line) for _, name, line in findings(flawfinder, old))
    added = []
    for found in findings(flawfinder, current):
        key: Key = (found[1], found[2])
        if old_keys[key] > 0:
            old_keys[key] -= 1
        else:
            added.append(found)
    return added


def merge_base(git: str, root: Path, branch: str) -> str:
    status, text = git_output(git, ["merge-base", "HEAD", branch], root)
    if status != 0 or not text.strip():
        raise ToolError(f"no merge base between HEAD and {branch}")
    return text.strip()


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").partition("\n")[0])
    parser.add_argument("--base-branch", default="origin/master")
    parser.add_argument("files", nargs="*", type=Path)
    args = parser.parse_args(argv)
    root = Path.cwd()
    try:
        flawfinder, git = tool("flawfinder"), tool("git")
        base = merge_base(git, root, args.base_branch)
        failed = 0
        for path in args.files:
            if path.suffix not in SUFFIXES or not path.is_file():
                continue
            relative = path.resolve().relative_to(root.resolve()).as_posix()
            for line, name, context in new_findings(
                flawfinder, path, base_text(git, root, base, relative)
            ):
                print(f"{relative}:{line}: flawfinder {name}: {context}")
                failed += 1
    except ToolError as error:
        print(f"check_flawfinder_new: {error}", file=sys.stderr)
        return 2
    if failed:
        print(
            f"\n{failed} new flawfinder finding(s); Codacy fails the pull request on each. "
            "Remove the call (see this script's docstring), do not suppress it.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
