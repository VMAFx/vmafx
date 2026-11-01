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

A pull request that changes what a hook checks also gets the whole tree: a
change to ``.pre-commit-config.yaml`` or to a file a local hook's ``entry``
names (its check script) can add findings in files the pull request never
touched. On the diff alone such a hook passed on its pull request and failed
the next master push (``check-fork-python-subprocess``, #3237 / a3ea48309).

Usage (workflow): ``precommit_scope.py --event "$EVENT" --base-sha "$BASE_SHA"``
(``--print`` shows the arguments without running anything).
"""

from __future__ import annotations

import argparse
import re
import shlex
import shutil
import sys
from collections.abc import Collection, Sequence
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

import yaml  # type: ignore[import-untyped]

from scripts.lib.safe_subprocess import run as run_command

ROOT = Path(__file__).resolve().parents[2]
CONFIG = ".pre-commit-config.yaml"

PULL_REQUEST_EVENTS = frozenset({"pull_request", "pull_request_target"})
SHA = re.compile(r"[0-9a-f]{40}")
BASE_ARGS = ("--show-diff-on-failure", "--color=always")
PRE_COMMIT_TIMEOUT_S = 1800.0
GIT_TIMEOUT_S = 120.0


def hook_inputs(config_text: str, root: Path = ROOT) -> frozenset[str]:
    """The config file and every repository file a local hook's ``entry`` names."""
    inputs = {CONFIG}
    for repo in (yaml.safe_load(config_text) or {}).get("repos", []):
        if repo.get("repo") != "local":
            continue
        for hook in repo.get("hooks", []):
            for token in shlex.split(str(hook.get("entry", "")), comments=True):
                if (root / token).is_file():
                    inputs.add(Path(token).as_posix())
    return frozenset(inputs)


def scope(
    event: str,
    base_sha: str,
    changed: Collection[str] | None = None,
    inputs: Collection[str] = (CONFIG,),
) -> tuple[list[str], str]:
    """Return the pre-commit selection arguments and the reason for them.

    ``changed`` is the pull request's file list; ``None`` (unknown) fails closed.
    """
    if event not in PULL_REQUEST_EVENTS:
        return ["--all-files"], f"event {event or 'unknown'} is not a pull request: whole tree"
    if not SHA.fullmatch(base_sha):
        return ["--all-files"], "pull request without a usable base sha: whole tree (fail closed)"
    if changed is None:
        return ["--all-files"], "pull request file list unreadable: whole tree (fail closed)"
    touched = sorted(set(changed) & set(inputs))
    if touched:
        return [
            "--all-files"
        ], f"pull request changes hook inputs ({', '.join(touched)}): whole tree"
    return (
        ["--from-ref", base_sha, "--to-ref", "HEAD"],
        f"pull request: files changed since {base_sha[:12]}; whole tree runs on master push,"
        " release tags, schedule and dispatch",
    )


def changed_files(base_sha: str) -> list[str] | None:
    """The files changed between ``base_sha`` and HEAD, or ``None`` when git fails."""
    git = shutil.which("git")
    if git is None or not SHA.fullmatch(base_sha):
        return None
    result = run_command(
        [git, "diff", "--name-only", "--no-renames", base_sha, "HEAD"],
        allowed_executables=(git,),
        cwd=ROOT,
        capture_output=True,
        text=True,
        timeout_seconds=GIT_TIMEOUT_S,
    )
    if result.returncode != 0:
        return None
    return [line for line in result.stdout.splitlines() if line]


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--event", default="")
    parser.add_argument("--base-sha", default="")
    parser.add_argument("--print", action="store_true", dest="print_only")
    args = parser.parse_args(argv)
    pull_request = args.event in PULL_REQUEST_EVENTS
    changed = changed_files(args.base_sha) if pull_request else []
    inputs = hook_inputs((ROOT / CONFIG).read_text(encoding="utf-8"))
    selection, reason = scope(args.event, args.base_sha, changed, inputs)
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
