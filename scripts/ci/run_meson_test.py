#!/usr/bin/env python3
# SPDX-License-Identifier: EUPL-1.2
# Copyright 2026 Lusoris
"""Run ``meson test`` after removing credential-bearing keys (ADR-1333)."""

from __future__ import annotations

import os
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


def sanitize_process_environment(environment: MutableMapping[str, str]) -> None:
    """Delete forbidden keys without inspecting or retaining their values."""
    for name in CREDENTIAL_ENV_VARS:
        if name in environment:
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
    try:
        # ADR-1333 requires same-process exec so no environment mapping is copied.
        os.execvp(command[0], command)  # noqa: S606
    except FileNotFoundError:
        print("run_meson_test.py: Meson executable not found", file=sys.stderr)
        return 127
    return 126


if __name__ == "__main__":
    raise SystemExit(main())
