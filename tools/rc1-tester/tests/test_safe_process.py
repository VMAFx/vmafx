# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the packaged process-group and output bounds."""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaf_rc1_tester.safe_process import (
    CommandOutputLimitExceeded,
    CommandTimedOut,
    CommandValidationError,
    run_bounded,
)


def test_real_child_timeout_is_fail_closed() -> None:
    with pytest.raises(CommandTimedOut, match="timed out"):
        run_bounded(
            [sys.executable, "-c", "import time; time.sleep(5)"],
            timeout_seconds=0.05,
            max_output_bytes=1024,
        )


def test_real_child_combined_output_limit_is_fail_closed() -> None:
    with pytest.raises(CommandOutputLimitExceeded) as exc:
        run_bounded(
            [sys.executable, "-c", "import sys; sys.stdout.write('x' * 2048)"],
            timeout_seconds=2.0,
            max_output_bytes=64,
        )
    assert len(exc.value.stdout.encode()) <= 64
    assert exc.value.stderr == ""


def test_relative_executable_path_is_rejected() -> None:
    with pytest.raises(CommandValidationError, match="absolute or bare"):
        run_bounded(
            ["build/tools/vmaf", "--version"],
            timeout_seconds=1.0,
            max_output_bytes=1024,
        )
