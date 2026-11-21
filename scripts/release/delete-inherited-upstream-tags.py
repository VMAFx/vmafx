#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Delete the release tags vidiko/vmafx inherited from Netflix/vmaf (ADR-1805).

A tag is selected only when Netflix/vmaf defines a tag of the same name that
points at the same object. A tag with an inherited-looking name on another
object is a fork tag and is refused. The default run changes nothing;
``--apply`` deletes through ``DELETE repos/vidiko/vmafx/git/refs/tags/<name>``.
Recreate a deleted tag with ``git tag <name> <sha> && git push origin
refs/tags/<name>`` using the object recorded in
``scripts/release/inherited-upstream-tags.json``.
"""

from __future__ import annotations

import argparse
import shutil
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from scripts.lib.safe_subprocess import CommandFailed
from scripts.lib.safe_subprocess import run as run_command

REPO = "vidiko/vmafx"
UPSTREAM_URL = "https://github.com/Netflix/vmaf"
TIMEOUT_SECONDS = 120
PEELED_SUFFIX = "^{}"
TAG_PREFIX = "refs/tags/"
LS_REMOTE_FIELDS = 2


@dataclass(frozen=True)
class Plan:
    """What the run selected, refused and left alone."""

    delete: tuple[str, ...]
    refused: tuple[tuple[str, str, str], ...]
    kept: tuple[str, ...]


def parse_ls_remote(text: str) -> dict[str, str]:
    """Map tag name to the tag object id (not the peeled commit)."""
    tags: dict[str, str] = {}
    for line in text.splitlines():
        parts = line.split()
        if len(parts) != LS_REMOTE_FIELDS or not parts[1].startswith(TAG_PREFIX):
            continue
        name = parts[1][len(TAG_PREFIX) :]
        if not name.endswith(PEELED_SUFFIX):
            tags[name] = parts[0]
    return tags


def select(fork: dict[str, str], upstream: dict[str, str]) -> Plan:
    """Select fork tags whose name and object equal Netflix's tag."""
    delete: list[str] = []
    refused: list[tuple[str, str, str]] = []
    kept: list[str] = []
    for name in sorted(fork):
        if name not in upstream:
            kept.append(name)
        elif fork[name] == upstream[name]:
            delete.append(name)
        else:
            refused.append((name, fork[name], upstream[name]))
    return Plan(tuple(delete), tuple(refused), tuple(kept))


def _git_ls_remote(remote: str) -> str:
    git = shutil.which("git")
    if git is None:
        raise SystemExit("git is not installed")
    result = run_command(
        [git, "ls-remote", "--tags", remote],
        allowed_executables=(git,),
        capture_output=True,
        text=True,
        timeout_seconds=TIMEOUT_SECONDS,
    )
    result.check_returncode()
    return str(result.stdout)


def _delete_ref(name: str) -> None:
    gh = shutil.which("gh")
    if gh is None:
        raise SystemExit("gh is not installed")
    endpoint = f"repos/{REPO}/git/refs/tags/{name}"
    result = run_command(
        [gh, "api", "--method", "DELETE", endpoint],
        allowed_executables=(gh,),
        capture_output=True,
        text=True,
        timeout_seconds=TIMEOUT_SECONDS,
    )
    result.check_returncode()


def report(plan: Plan, apply: bool) -> None:
    """Print the plan; the verb says whether anything was changed."""
    verb = "delete" if apply else "would delete"
    for name in plan.delete:
        print(f"{verb} {name}")
    for name, fork_sha, upstream_sha in plan.refused:
        print(f"REFUSE {name}: fork {fork_sha} differs from Netflix {upstream_sha}")
    print(f"keep {len(plan.kept)} fork tags: {' '.join(plan.kept)}")


def run(
    fork_listing: str,
    upstream_listing: str,
    apply: bool,
    delete_ref: Callable[[str], None] = _delete_ref,
) -> int:
    """Plan, report and (with apply) delete; refusals make the exit status 2."""
    plan = select(parse_ls_remote(fork_listing), parse_ls_remote(upstream_listing))
    report(plan, apply)
    if plan.refused:
        return 2
    if apply:
        for name in plan.delete:
            delete_ref(name)
    return 0


def parse_args(argv: Sequence[str]) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--apply", action="store_true", help="delete (default: dry run)")
    parser.add_argument("--fork-remote", default="origin")
    parser.add_argument("--upstream-url", default=UPSTREAM_URL)
    return parser.parse_args(argv)


def main(argv: Sequence[str] | None = None) -> int:
    args = parse_args(list(sys.argv[1:] if argv is None else argv))
    try:
        fork = _git_ls_remote(args.fork_remote)
        upstream = _git_ls_remote(args.upstream_url)
        return run(fork, upstream, args.apply)
    except CommandFailed as error:
        print(f"delete-inherited-upstream-tags: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
