#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Tests for the shell-free command runner used by testdata utilities."""

from __future__ import annotations

import math
import os
import subprocess
import sys
from pathlib import Path

import pytest

from testdata._command import run_command

FAILURE_EXIT_CODE = 7


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
            [
                sys.executable,
                "-c",
                f"import sys; print('failed', file=sys.stderr); sys.exit({FAILURE_EXIT_CODE})",
            ],
            capture_output=True,
            text=True,
            check=True,
        )

    assert failure.value.returncode == FAILURE_EXIT_CODE
    assert failure.value.stderr == "failed\n"


def test_run_command_preserves_partial_timeout_output() -> None:
    with pytest.raises(subprocess.TimeoutExpired) as failure:
        run_command(
            [
                sys.executable,
                "-c",
                "import sys, time; print('waiting', file=sys.stderr, flush=True); time.sleep(60)",
            ],
            capture_output=True,
            text=True,
            timeout=0.2,
        )

    stderr: object = failure.value.stderr
    assert stderr == "waiting\n"


@pytest.mark.parametrize("timeout", [0.0, -1.0, math.inf, math.nan])
def test_run_command_rejects_unbounded_timeout(timeout: float) -> None:
    with pytest.raises(ValueError, match="positive and finite"):
        run_command([sys.executable, "-c", "pass"], timeout=timeout)


def test_run_command_resolves_program_from_supplied_path() -> None:
    environment = dict(os.environ)
    environment["PATH"] = str(Path(sys.executable).parent)

    result = run_command(
        [Path(sys.executable).name, "-c", "print('resolved')"],
        capture_output=True,
        text=True,
        env=environment,
    )

    assert result.stdout == "resolved\n"


def test_run_command_rejects_missing_program() -> None:
    with pytest.raises(FileNotFoundError, match="executable not found"):
        run_command(["vmafx-program-that-does-not-exist"])
