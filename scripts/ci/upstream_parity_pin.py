#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Print the Netflix/vmaf commit the fork records itself at parity with.

The record is one heading in docs/development/known-upstream-bugs.md:

    ## Upstream head the fork is at parity with: `<commit id>` (<date>)

An upstream port or sync moves it. The licence provenance job (ADR-1474) reads
it so the upstream tree it compares against changes only with a reviewed pull
request, never because Netflix pushed a commit.

Without options the recorded id is printed as written. With ``--within REF``
the id is also resolved in the checkout: it must name exactly one commit, and
REF (the fetched upstream branch) must contain it. ``--url`` prints the shared
Netflix URL instead, taking ``NETFLIX_VMAF_URL`` from the environment when set
and from ``build-config.env`` otherwise. The full id is printed.

Exit status: 0 on success, 2 when the record is missing, ambiguous, malformed,
or does not name a commit of the upstream branch.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import sys
from collections.abc import Callable, Sequence
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO))

from scripts.lib.safe_subprocess import run as run_command  # noqa: E402

RECORD = Path("docs/development/known-upstream-bugs.md")
CONFIG = Path("build-config.env")
NETFLIX_URL_KEY = "NETFLIX_VMAF_URL"
HEADING = "## Upstream head the fork is at parity with"
PIN_LINE = re.compile(
    r"^## Upstream head the fork is at parity with: `([0-9a-f]{7,40})`"
    r"(?: \(\d{4}-\d{2}-\d{2}\))?$"
)
GIT_TIMEOUT_SECONDS = 60

# (argv after "git -C <repo>") -> (exit status, stdout)
GitRunner = Callable[[Sequence[str]], tuple[int, str]]


class PinError(ValueError):
    """The record does not name one upstream commit."""


def recorded_pin(text: str) -> str:
    """The commit id of the one parity heading in *text*."""
    headings = [line.rstrip() for line in text.splitlines() if line.startswith(HEADING)]
    if not headings:
        raise PinError(f"no heading '{HEADING}: `<commit id>`' in {RECORD}")
    if len(headings) > 1:
        raise PinError(
            f"{len(headings)} headings name the upstream head in {RECORD}; keep one "
            "(give the older section another title)"
        )
    match = PIN_LINE.match(headings[0])
    if match is None:
        raise PinError(
            f"malformed heading in {RECORD}: {headings[0]!r}; expected "
            f"'{HEADING}: `<7 to 40 lowercase hex digits>` (<YYYY-MM-DD>)'"
        )
    return match.group(1)


def git_runner(repo: Path) -> GitRunner:
    """A runner for git in *repo*."""
    git = shutil.which("git")
    if git is None:
        raise PinError("git is not installed")

    def call(args: Sequence[str]) -> tuple[int, str]:
        proc = run_command(
            [git, "-C", str(repo), *args],
            allowed_executables=(git,),
            capture_output=True,
            text=True,
            check=False,
            timeout_seconds=GIT_TIMEOUT_SECONDS,
        )
        return proc.returncode, proc.stdout.strip()

    return call


def resolve(pin: str, within: str, call: GitRunner) -> str:
    """The full id of *pin*, which *within* must contain."""
    status, full = call(["rev-parse", "--verify", "--quiet", f"{pin}^{{commit}}"])
    if status != 0 or not re.fullmatch(r"[0-9a-f]{40,64}", full):
        raise PinError(
            f"`{pin}` does not name exactly one commit in this checkout; fetch "
            "Netflix/vmaf with its full history, or record a longer id"
        )
    status, _ = call(["merge-base", "--is-ancestor", full, within])
    if status != 0:
        raise PinError(f"{within} does not contain {full}: it is not an upstream commit")
    return full


def upstream_url(repo: Path = REPO) -> str:
    """The shared Netflix URL, with ``NETFLIX_VMAF_URL`` as an explicit mirror override."""
    override = os.environ.get(NETFLIX_URL_KEY)
    if override:
        return override
    config = repo / CONFIG
    if not config.is_file():
        raise PinError(f"{config} not found")
    prefix = f'{NETFLIX_URL_KEY}="'
    for line in config.read_text(encoding="utf-8").splitlines():
        if line.startswith(prefix) and line.endswith('"'):
            return line[len(prefix) : -1]
    raise PinError(f"{config} declares no {NETFLIX_URL_KEY}")


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=(__doc__ or "").split("\n", 1)[0])
    parser.add_argument("--repo", type=Path, default=REPO, help="checkout (default: this one)")
    parser.add_argument(
        "--within", metavar="REF", help="resolve the id and require REF to contain the commit"
    )
    parser.add_argument(
        "--url", action="store_true", help="print the shared Netflix URL instead of the pin"
    )
    args = parser.parse_args(argv)
    try:
        if args.url:
            print(upstream_url(args.repo))
            return 0
        pin = recorded_pin((args.repo / RECORD).read_text(encoding="utf-8"))
        if args.within:
            pin = resolve(pin, args.within, git_runner(args.repo))
    except (OSError, PinError) as error:
        print(f"upstream_parity_pin: {error}", file=sys.stderr)
        return 2
    print(pin)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
