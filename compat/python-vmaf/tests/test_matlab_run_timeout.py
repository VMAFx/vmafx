# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for _run_matlab timeout and error handling in
# compat/python-vmaf/core/matlab_feature_extractor.py.
"""Pytest cases for _run_matlab timeout and process error handling."""

from __future__ import annotations

import subprocess
from pathlib import Path
from typing import Any

import pytest

from vmaf.core import matlab_feature_extractor
from vmaf.core.matlab_feature_extractor import SCRIPT_TIMEOUT_S, _run_matlab


def test_run_matlab_positive_no_log_records_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """Verify check_output receives the configured timeout when no log path is provided."""
    recorded_kwargs: dict[str, Any] = {}

    def fake_check_output(*args: Any, **kwargs: Any) -> bytes:
        """Record check_output kwargs and return empty bytes."""
        recorded_kwargs.update(kwargs)
        return b""

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "check_output", fake_check_output)
    _run_matlab("matlab", "test_script")
    assert recorded_kwargs.get("timeout") == SCRIPT_TIMEOUT_S


def test_run_matlab_positive_with_log_records_timeout(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Verify subprocess.run receives the configured timeout when a log path is provided."""
    recorded_kwargs: dict[str, Any] = {}

    def fake_run(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
        """Record run kwargs and return a successful CompletedProcess."""
        recorded_kwargs.update(kwargs)
        return subprocess.CompletedProcess(args[0] if args else [], 0, stdout=b"", stderr=b"")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "run", fake_run)
    _run_matlab("matlab", "test_script", log_file_path=tmp_path / "log")
    assert recorded_kwargs.get("timeout") == SCRIPT_TIMEOUT_S


def test_run_matlab_negative_called_process_error(monkeypatch: pytest.MonkeyPatch) -> None:
    """Verify CalledProcessError converts to AssertionError matching process returncode."""
    argv = ["matlab", "-nodisplay", "-nosplash", "-nodesktop", "-r", "test_script"]

    def fake_check_output(*args: Any, **kwargs: Any) -> bytes:
        """Simulate check_output failing with a CalledProcessError."""
        raise subprocess.CalledProcessError(3, argv, output=b"x")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "check_output", fake_check_output)
    with pytest.raises(AssertionError, match=r"Process returned 3"):
        _run_matlab("matlab", "test_script")


def test_run_matlab_negative_called_process_error_with_log(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Verify CalledProcessError with log path converts to AssertionError matching returncode."""
    argv = ["matlab", "-nodisplay", "-nosplash", "-nodesktop", "-r", "test_script"]

    def fake_run(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
        """Simulate run failing with a CalledProcessError."""
        raise subprocess.CalledProcessError(3, argv, output=b"x")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "run", fake_run)
    with pytest.raises(AssertionError, match=r"Process returned 3"):
        _run_matlab("matlab", "test_script", log_file_path=tmp_path / "log")


def test_run_matlab_boundary_timeout_no_log(monkeypatch: pytest.MonkeyPatch) -> None:
    """Verify TimeoutExpired without log file converts to AssertionError with timeout details."""
    argv = ["matlab", "-nodisplay", "-nosplash", "-nodesktop", "-r", "test_script"]

    def fake_check_output(*args: Any, **kwargs: Any) -> bytes:
        """Simulate check_output raising a TimeoutExpired exception."""
        raise subprocess.TimeoutExpired(argv, SCRIPT_TIMEOUT_S, output=b"partial")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "check_output", fake_check_output)
    with pytest.raises(AssertionError) as exc_info:
        _run_matlab("matlab", "test_script")
    msg = str(exc_info.value)
    assert "timed out after" in msg
    assert str(SCRIPT_TIMEOUT_S) in msg
    assert "SCRIPT_TIMEOUT_S" in msg


def test_run_matlab_boundary_timeout_with_log(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Verify TimeoutExpired with log file converts to AssertionError with timeout details."""
    argv = ["matlab", "-nodisplay", "-nosplash", "-nodesktop", "-r", "test_script"]

    def fake_run(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
        """Simulate run raising a TimeoutExpired exception."""
        raise subprocess.TimeoutExpired(argv, SCRIPT_TIMEOUT_S, output=b"partial")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "run", fake_run)
    with pytest.raises(AssertionError) as exc_info:
        _run_matlab("matlab", "test_script", log_file_path=tmp_path / "log")
    msg = str(exc_info.value)
    assert "timed out after" in msg
    assert str(SCRIPT_TIMEOUT_S) in msg
    assert "SCRIPT_TIMEOUT_S" in msg
