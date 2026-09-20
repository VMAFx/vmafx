# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Tests for the developer CLI process boundary."""

from __future__ import annotations

import math
import os
import subprocess
import sys
from pathlib import Path

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


def test_checked_output_preserves_text_failure_output() -> None:
    with pytest.raises(subprocess.CalledProcessError) as failure:
        checked_output(
            [sys.executable, "-c", "import sys; print('failed', file=sys.stderr); sys.exit(7)"],
            text=True,
        )

    assert failure.value.output == ""
    assert failure.value.stderr == "failed\n"


def test_checked_output_enforces_timeout() -> None:
    with pytest.raises(subprocess.TimeoutExpired) as failure:
        checked_output(
            [
                sys.executable,
                "-c",
                "import sys, time; print('waiting', file=sys.stderr, flush=True); time.sleep(60)",
            ],
            timeout=0.5,
        )

    assert failure.value.stderr == b"waiting\n"


def test_checked_output_rejects_empty_command() -> None:
    with pytest.raises(ValueError, match="must not be empty"):
        checked_output([])


@pytest.mark.parametrize("timeout", [0.0, -1.0, math.inf, math.nan])
def test_checked_output_rejects_unbounded_timeout(timeout: float) -> None:
    with pytest.raises(ValueError, match="positive and finite"):
        checked_output([sys.executable, "-c", "pass"], timeout=timeout)


def test_checked_output_resolves_program_from_supplied_path() -> None:
    environment = dict(os.environ)
    environment["PATH"] = str(Path(sys.executable).parent)

    assert (
        checked_output(
            [Path(sys.executable).name, "-c", "print('resolved')"],
            text=True,
            env=environment,
        )
        == "resolved\n"
    )


def test_checked_output_rejects_missing_program() -> None:
    with pytest.raises(FileNotFoundError, match="executable not found"):
        checked_output(["vmafx-program-that-does-not-exist"])
