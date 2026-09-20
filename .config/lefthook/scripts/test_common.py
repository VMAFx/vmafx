"""Regression tests for the hook process boundary."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

import pytest

COMMON_PATH = Path(__file__).with_name("common.py")
COMMON_SPEC = importlib.util.spec_from_file_location("lefthook_common", COMMON_PATH)
assert COMMON_SPEC is not None and COMMON_SPEC.loader is not None
common = importlib.util.module_from_spec(COMMON_SPEC)
sys.modules[COMMON_SPEC.name] = common
COMMON_SPEC.loader.exec_module(common)

FAILURE_EXIT_CODE = 7


def test_run_bounded_captures_stdout() -> None:
    output = common.run_bounded(
        [sys.executable, "-c", "print('ready')"], timeout=5, max_output=1024
    )

    assert output == b"ready\n"


def test_run_bounded_enforces_combined_output_limit() -> None:
    with pytest.raises(common.HookError, match="output exceeded"):
        common.run_bounded(
            [sys.executable, "-c", "import sys; sys.stdout.write('x' * 2048)"],
            timeout=5,
            max_output=1024,
        )


def test_run_bounded_enforces_timeout() -> None:
    with pytest.raises(common.HookError, match="timed out"):
        common.run_bounded(
            [sys.executable, "-c", "import time; time.sleep(60)"],
            timeout=0.05,
            max_output=1024,
        )


def test_run_bounded_does_not_copy_failure_diagnostics() -> None:
    with pytest.raises(common.HookError) as failure:
        common.run_bounded(
            [
                sys.executable,
                "-c",
                f"import sys; print('credential-shaped', file=sys.stderr); "
                f"raise SystemExit({FAILURE_EXIT_CODE})",
            ],
            timeout=5,
            max_output=1024,
        )

    assert "credential-shaped" not in str(failure.value)
    assert str(FAILURE_EXIT_CODE) in str(failure.value)


def test_run_preserves_input_and_checked_diagnostics() -> None:
    assert (
        common.run(
            [sys.executable, "-c", "import sys; sys.stdout.buffer.write(sys.stdin.buffer.read())"],
            data=b"payload",
            timeout=5,
        )
        == b"payload"
    )

    with pytest.raises(common.HookError, match="visible failure"):
        common.run(
            [
                sys.executable,
                "-c",
                f"import sys; print('visible failure', file=sys.stderr); "
                f"raise SystemExit({FAILURE_EXIT_CODE})",
            ],
            timeout=5,
        )


def test_empty_argv_fails_before_spawn() -> None:
    with pytest.raises(common.HookError, match="argv is empty"):
        common.run_bounded([])
