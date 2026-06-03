# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for aiutils.subprocess_utils.run_cmd.

Exercises capture mode, timeout, and the check=False / check=True branches
without spawning long-lived processes.
"""

from __future__ import annotations

import subprocess
import sys

import pytest

from aiutils.subprocess_utils import run_cmd


def test_run_cmd_capture_returns_stdout_text() -> None:
    """capture=True wires capture_output + text so stdout is a string."""
    result = run_cmd([sys.executable, "-c", "print('hello')"], capture=True)
    assert result.returncode == 0
    assert result.stdout.strip() == "hello"
    assert isinstance(result.stdout, str)


def test_run_cmd_check_raises_on_non_zero() -> None:
    """check=True (default) raises CalledProcessError on a non-zero exit."""
    with pytest.raises(subprocess.CalledProcessError):
        run_cmd([sys.executable, "-c", "import sys; sys.exit(7)"])


def test_run_cmd_check_false_returns_non_zero() -> None:
    """check=False lets the caller inspect a non-zero returncode without raising."""
    result = run_cmd([sys.executable, "-c", "import sys; sys.exit(3)"], check=False, capture=True)
    assert result.returncode == 3


def test_run_cmd_timeout_propagates() -> None:
    """A short timeout against a sleeping subprocess raises TimeoutExpired."""
    with pytest.raises(subprocess.TimeoutExpired):
        run_cmd(
            [sys.executable, "-c", "import time; time.sleep(5)"],
            timeout=0.5,
        )


def test_run_cmd_passes_through_kwargs() -> None:
    """Extra kwargs (cwd, env) reach subprocess.run unchanged."""
    result = run_cmd(
        [sys.executable, "-c", "import os; print(os.getcwd())"],
        capture=True,
        cwd="/tmp",
    )
    assert result.stdout.strip() == "/tmp"
