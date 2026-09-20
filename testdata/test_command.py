#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Unit tests for the shell-free command runner used by benchmark scripts."""

import subprocess
import sys

import pytest

from testdata._command import run_command

FAILURE_EXIT_CODE = 7
COMMAND_TIMEOUT_SECONDS = 0.05


def test_run_command_captures_text_output() -> None:
    result = run_command(
        [sys.executable, "-c", "print('ready')"],
        capture_output=True,
        text=True,
        check=True,
    )

    assert result.returncode == 0
    assert result.stdout == "ready\n"
    assert result.stderr == ""


def test_run_command_preserves_checked_failure() -> None:
    with pytest.raises(subprocess.CalledProcessError) as failure:
        run_command(
            [sys.executable, "-c", f"raise SystemExit({FAILURE_EXIT_CODE})"],
            capture_output=True,
            text=True,
            check=True,
        )

    assert failure.value.returncode == FAILURE_EXIT_CODE


def test_run_command_preserves_timeout_error() -> None:
    with pytest.raises(subprocess.TimeoutExpired):
        run_command(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            timeout=COMMAND_TIMEOUT_SECONDS,
        )
