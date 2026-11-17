#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run `git clone` with a bounded retry for transient connection failures.

The CI downloads use one helper so every network clone retries the same way
(issue 3392): at most three attempts, pausing one, then two, seconds between
them. The clone arguments are passed through unchanged, so a caller keeps its
branch, depth and destination exactly as before.

Usage:
    git_clone_retry.py [--attempts N] [--delay SECONDS] -- GIT_CLONE_ARGS...

`--attempts` must be 1..10; `--delay` defaults to 10 seconds and may be zero.
The exit status is the last clone's status, or 2 for a usage error.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time

DEFAULT_ATTEMPTS = 3
DEFAULT_DELAY = 10
MAX_ATTEMPTS = 10


def parse_args(argv: list[str]) -> tuple[int, int, list[str]]:
    """Split the helper's options from the arguments passed to git."""
    attempts = DEFAULT_ATTEMPTS
    delay = DEFAULT_DELAY
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument.startswith("--attempts="):
            value = argument.split("=", 1)[1]
            index += 1
        elif argument == "--attempts":
            if index + 1 >= len(argv):
                raise ValueError("--attempts needs a value")
            value = argv[index + 1]
            index += 2
        elif argument.startswith("--delay="):
            value = argument.split("=", 1)[1]
            delay = _non_negative(value, "--delay")
            index += 1
            continue
        elif argument == "--delay":
            if index + 1 >= len(argv):
                raise ValueError("--delay needs a value")
            delay = _non_negative(argv[index + 1], "--delay")
            index += 2
            continue
        elif argument == "--":
            index += 1
            break
        else:
            break
        attempts = _positive(value, "--attempts")
    if index == len(argv):
        raise ValueError("no git clone arguments")
    return attempts, delay, argv[index:]


def _positive(value: str, option: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise ValueError(f"{option} must be a positive integer, got {value!r}") from error
    if number < 1 or number > MAX_ATTEMPTS:
        raise ValueError(f"{option} must be 1..{MAX_ATTEMPTS}, got {number}")
    return number


def _non_negative(value: str, option: str) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise ValueError(f"{option} must be a non-negative integer, got {value!r}") from error
    if number < 0:
        raise ValueError(f"{option} must be non-negative, got {number}")
    return number


def main(argv: list[str] | None = None) -> int:
    argv = sys.argv[1:] if argv is None else argv
    try:
        attempts, delay, clone_args = parse_args(argv)
    except ValueError as error:
        print(f"git_clone_retry: {error}", file=sys.stderr)
        return 2
    git = os.environ.get("GIT_BIN") or shutil.which("git") or "git"
    status = 1
    for attempt in range(1, attempts + 1):
        status = subprocess.run([git, "clone", *clone_args], check=False).returncode  # noqa: S603
        if status == 0:
            return 0
        if attempt < attempts:
            pause = attempt * delay
            print(
                f"git_clone_retry: attempt {attempt} of {attempts} failed "
                f"(status {status}); retrying in {pause}s",
                file=sys.stderr,
            )
            if pause:
                time.sleep(pause)
    print(
        f"git_clone_retry: git clone failed {attempts} times, last status {status}", file=sys.stderr
    )
    return status


if __name__ == "__main__":
    raise SystemExit(main())
