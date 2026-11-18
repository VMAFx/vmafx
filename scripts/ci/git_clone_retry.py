#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Run `git clone` with a bounded retry for transient connection failures.

The CI downloads use one helper so every network clone retries the same way
(issue 3392): at most three attempts, pausing one, then two, seconds between
them. The clone arguments are passed through unchanged, so a caller keeps its
branch, depth and destination exactly as before.

Usage:
    git_clone_retry.py [--attempts N] [--delay SECONDS] [--timeout SECONDS] -- GIT_CLONE_ARGS...

`--attempts` must be 1..10; `--delay` defaults to 1 second and may be zero.
`--timeout` defaults to 600 seconds and must be 1..3600.
The exit status is the last clone's status, 124 when a clone times out, or 2
for a usage error.
"""

from __future__ import annotations

import os
import shutil
import subprocess
import sys
import time

DEFAULT_ATTEMPTS = 3
DEFAULT_DELAY = 1
DEFAULT_TIMEOUT = 600
MAX_ATTEMPTS = 10
MAX_TIMEOUT = 3600
HELPER_OPTIONS = frozenset({"--attempts", "--delay", "--timeout"})


def _option(argv: list[str], index: int) -> tuple[str, str, int]:
    argument = argv[index]
    if argument.startswith("--") and "=" in argument:
        name, value = argument.split("=", 1)
        return name, value, index + 1
    if index + 1 >= len(argv):
        raise ValueError(f"{argument} needs a value")
    return argument, argv[index + 1], index + 2


def parse_args(argv: list[str]) -> tuple[int, int, int, list[str]]:
    """Split the helper's options from the arguments passed to git."""
    attempts = DEFAULT_ATTEMPTS
    delay = DEFAULT_DELAY
    timeout = DEFAULT_TIMEOUT
    index = 0
    while index < len(argv):
        argument = argv[index]
        if argument == "--":
            index += 1
            break
        if argument.split("=", 1)[0] not in HELPER_OPTIONS:
            break
        name, value, index = _option(argv, index)
        if name == "--attempts":
            attempts = _bounded(value, name, MAX_ATTEMPTS)
        elif name == "--delay":
            delay = _non_negative(value, name)
        else:
            timeout = _bounded(value, name, MAX_TIMEOUT)
    if index == len(argv):
        raise ValueError("no git clone arguments")
    return attempts, delay, timeout, argv[index:]


def _bounded(value: str, option: str, maximum: int) -> int:
    try:
        number = int(value)
    except ValueError as error:
        raise ValueError(f"{option} must be a positive integer, got {value!r}") from error
    if number < 1 or number > maximum:
        raise ValueError(f"{option} must be 1..{maximum}, got {number}")
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
        attempts, delay, timeout, clone_args = parse_args(argv)
    except ValueError as error:
        print(f"git_clone_retry: {error}", file=sys.stderr)
        return 2
    git = os.environ.get("GIT_BIN") or shutil.which("git") or "git"
    status = 1
    for attempt in range(1, attempts + 1):
        try:
            # nosemgrep: python.lang.security.audit.dangerous-subprocess-use-tainted-env-args
            # argv form, no shell: clone_args are workflow-owned git arguments,
            # never interpolated into a command string.
            status = subprocess.run(  # noqa: S603
                [git, "clone", *clone_args], check=False, timeout=timeout
            ).returncode
        except subprocess.TimeoutExpired:
            status = 124
            print(
                f"git_clone_retry: attempt {attempt} of {attempts} timed out after " f"{timeout}s",
                file=sys.stderr,
            )
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
        f"git_clone_retry: git clone failed {attempts} times, last status {status}",
        file=sys.stderr,
    )
    return status


if __name__ == "__main__":
    raise SystemExit(main())
