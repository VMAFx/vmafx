# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Tests for the developer CLI process boundary."""

from __future__ import annotations

import subprocess
import sys

import pytest

from vmaf_dev_llm.process import checked_output

FAILURE_EXIT_CODE = 7


def test_checked_output_returns_text() -> None:
    assert checked_output([sys.executable, "-c", "print('ready')"], text=True) == "ready\n"


def test_checked_output_preserves_failure_output() -> None:
    with pytest.raises(subprocess.CalledProcessError) as failure:
        checked_output(
            [
                sys.executable,
                "-c",
                f"import sys; print('failed', file=sys.stderr); raise SystemExit({FAILURE_EXIT_CODE})",
            ]
        )

    assert failure.value.returncode == FAILURE_EXIT_CODE
    assert failure.value.stderr == b"failed\n"


def test_checked_output_enforces_timeout() -> None:
    with pytest.raises(subprocess.TimeoutExpired):
        checked_output(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            timeout=0.05,
        )


def test_checked_output_rejects_empty_command() -> None:
    with pytest.raises(ValueError, match="must not be empty"):
        checked_output([])
