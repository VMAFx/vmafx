#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Run ``meson test`` after removing credential-bearing keys (ADR-1333)."""

from __future__ import annotations

import os
import re
import shutil
import subprocess
import sys
from collections.abc import MutableMapping, Sequence

CREDENTIAL_ENV_VARS = (
    "GITHUB_PERSONAL_ACCESS_TOKEN",
    "GITHUB_TOKEN",
    "GH_TOKEN",
    "GH_ENTERPRISE_TOKEN",
    "GITHUB_ENTERPRISE_TOKEN",
    "GITHUB_PAT",
    "GH_PAT",
    "GITHUB_AUTH_TOKEN",
    "GITHUB_API_TOKEN",
    "HOMEBREW_GITHUB_API_TOKEN",
    "ACTIONS_ID_TOKEN_REQUEST_TOKEN",
    "ACTIONS_RUNTIME_TOKEN",
)
SUBPROCESS_TIMEOUT_S = 3600

# ADR-1333 amendment (2026-11-05): a fixed list misses credentials exported under
# other names (a messaging token, a gateway key, a personal ``*_PAT``), and Meson
# then writes them to testlog.txt and testlog.json. Names matching this pattern are
# removed as well. Only names are matched; values are never read.
CREDENTIAL_NAME_PATTERN = re.compile(
    r"TOKEN(?!IZER)|SECRET|PASSWORD|PASSWD|CREDENTIAL|API_?KEY|ACCESS_KEY|PRIVATE_KEY"
    r"|(?:^|_)PAT$|(?:^|_)AUTH(?:_|$)",
    re.IGNORECASE,
)


def is_credential_name(name: str) -> bool:
    """Return whether ``name`` is a listed or pattern-matched credential key."""
    return name in CREDENTIAL_ENV_VARS or CREDENTIAL_NAME_PATTERN.search(name) is not None


def sanitize_process_environment(environment: MutableMapping[str, str]) -> None:
    """Delete credential keys without inspecting or retaining their values."""
    for name in [key for key in environment if is_credential_name(key)]:
        del environment[name]


def _command(argv: Sequence[str]) -> list[str]:
    """Build the Meson command without invoking a shell."""
    arguments = list(argv)
    meson_executable = "meson"
    if arguments[:1] == ["--meson-executable"]:
        try:
            meson_executable = arguments[1]
        except IndexError as exc:
            raise ValueError("missing Meson executable") from exc
        if not meson_executable:
            raise ValueError("missing Meson executable")
        arguments = arguments[2:]
    if arguments[:1] == ["--"]:
        arguments = arguments[1:]
    return [meson_executable, "test", *arguments]


def main(argv: Sequence[str] | None = None) -> int:
    """Sanitize this process, then replace it with Meson's test runner."""
    try:
        command = _command(sys.argv[1:] if argv is None else argv)
    except ValueError:
        print("run_meson_test.py: --meson-executable requires a value", file=sys.stderr)
        return 2

    sanitize_process_environment(os.environ)
    if os.name == "nt":
        return _run_and_wait(command)
    try:
        # ADR-1333 requires same-process exec so no environment mapping is copied.
        os.execvp(command[0], command)  # noqa: S606
    except FileNotFoundError:
        print("run_meson_test.py: Meson executable not found", file=sys.stderr)
        return 127
    return 126


def _run_and_wait(command: list[str]) -> int:
    """Run Meson as a child on Windows and return its exit status.

    Windows has no exec: the CRT's ``_execvp`` starts the new program and
    ends this process at once with status 0, so every caller saw success while
    ``meson test`` was still running, and CI moved on after the first tests
    (ADR-1364). The child inherits this process's already-sanitized
    environment block; no mapping is passed or copied.
    """
    executable = shutil.which(command[0])
    if executable is None:
        print("run_meson_test.py: Meson executable not found", file=sys.stderr)
        return 127
    try:
        completed = subprocess.run(  # noqa: S603
            [executable, *command[1:]],
            check=False,
            timeout=SUBPROCESS_TIMEOUT_S,
        )
        return completed.returncode
    except subprocess.TimeoutExpired:
        print(
            f"run_meson_test.py: Meson timed out after {SUBPROCESS_TIMEOUT_S}s",
            file=sys.stderr,
        )
        return 124


if __name__ == "__main__":
    raise SystemExit(main())
