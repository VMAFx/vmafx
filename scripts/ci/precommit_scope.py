#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Decide how much of the tree the Pre-Commit CI job checks, and run it (HISS-18).

A pull request is checked on its own diff: ``pre-commit run --from-ref <base sha>
--to-ref HEAD`` runs each hook on the files the pull request changed, so a
documentation-only change never pays for the C, Go and Rust hooks. Every other
event (master push, release tag, schedule, dispatch) and any pull request whose
base commit is missing keep the whole tree: ``--all-files``. The required
context ``Pre-Commit`` is reported by the job either way; the scope is printed
with its reason so a narrowed run is never mistaken for a full one.

Usage (workflow): ``precommit_scope.py --event "$EVENT" --base-sha "$BASE_SHA"``
(``--print`` shows the arguments without running anything).
"""

from __future__ import annotations

import argparse
import re
import shutil
import sys
from collections.abc import Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import run as run_command

PULL_REQUEST_EVENTS = frozenset({"pull_request", "pull_request_target"})
SHA = re.compile(r"[0-9a-f]{40}")
BASE_ARGS = ("--show-diff-on-failure", "--color=always")
PRE_COMMIT_TIMEOUT_S = 1800.0


def scope(event: str, base_sha: str) -> tuple[list[str], str]:
    """Return the pre-commit selection arguments and the reason for them."""
    if event not in PULL_REQUEST_EVENTS:
        return ["--all-files"], f"event {event or 'unknown'} is not a pull request: whole tree"
    if not SHA.fullmatch(base_sha):
        return ["--all-files"], "pull request without a usable base sha: whole tree (fail closed)"
    return (
        ["--from-ref", base_sha, "--to-ref", "HEAD"],
        f"pull request: files changed since {base_sha[:12]}; whole tree runs on master push,"
        " release tags, schedule and dispatch",
    )


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", default="")
    parser.add_argument("--base-sha", default="")
    parser.add_argument("--print", action="store_true", dest="print_only")
    args = parser.parse_args(argv)
    selection, reason = scope(args.event, args.base_sha)
    print(f"precommit-scope: {reason}", file=sys.stderr)
    command = ["pre-commit", "run", *BASE_ARGS, *selection]
    if args.print_only:
        print(" ".join(command))
        return 0
    executable = shutil.which("pre-commit")
    if executable is None:
        print("precommit-scope: pre-commit not found", file=sys.stderr)
        return 2
    result = run_command(
        [executable, *command[1:]],
        allowed_executables=(executable,),
        text=True,
        timeout_seconds=PRE_COMMIT_TIMEOUT_S,
    )
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
