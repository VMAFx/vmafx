# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for _run_matlab timeout and error handling in
# compat/python-vmaf/core/matlab_feature_extractor.py.
"""Pytest cases for _run_matlab timeout and process error handling."""

from __future__ import annotations

import subprocess  # nosec B404 -- construct subprocess results and exceptions for test doubles.
from pathlib import Path
from typing import Any
from types import SimpleNamespace
from unittest import mock

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
    with pytest.raises(AssertionError, match=r"Process returned 3") as exc_info:
        _run_matlab("matlab", "test_script")
    assert isinstance(exc_info.value.__cause__, subprocess.CalledProcessError)
    assert "msg: x" in str(exc_info.value)
    assert "b'x'" not in str(exc_info.value)


def test_run_matlab_negative_called_process_error_with_log(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Verify CalledProcessError with log path converts to AssertionError matching returncode."""
    argv = ["matlab", "-nodisplay", "-nosplash", "-nodesktop", "-r", "test_script"]

    def fake_run(*args: Any, **kwargs: Any) -> subprocess.CompletedProcess[bytes]:
        """Simulate run failing with a CalledProcessError."""
        raise subprocess.CalledProcessError(3, argv, output=b"x")

    monkeypatch.setattr(matlab_feature_extractor.subprocess, "run", fake_run)
    with pytest.raises(AssertionError, match=r"Process returned 3") as exc_info:
        _run_matlab("matlab", "test_script", log_file_path=tmp_path / "log")
    assert isinstance(exc_info.value.__cause__, subprocess.CalledProcessError)
    assert "msg: x" in str(exc_info.value)


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
    assert isinstance(exc_info.value.__cause__, subprocess.TimeoutExpired)
    assert "msg: partial" in msg
    assert "b'partial'" not in msg


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
    assert isinstance(exc_info.value.__cause__, subprocess.TimeoutExpired)
    assert "msg: partial" in msg
    assert "b'partial'" not in msg


@pytest.mark.parametrize("output, expected", [(b"text\xff", "text�"), (None, ""), ("text", "text")])
def test_matlab_error_output_is_readable(output: str | bytes | None, expected: str) -> None:
    """Handle invalid bytes, absent output and decoded strings consistently."""
    assert matlab_feature_extractor._matlab_output(output) == expected


@pytest.mark.parametrize(
    "extractor_type",
    [
        matlab_feature_extractor.StrredFeatureExtractor,
        matlab_feature_extractor.StrredOptFeatureExtractor,
        matlab_feature_extractor.SpEEDMatlabFeatureExtractor,
        matlab_feature_extractor.STMADFeatureExtractor,
        matlab_feature_extractor.iCIDFeatureExtractor,
    ],
)
def test_generate_result_restores_cwd_on_timeout(tmp_path: Path, extractor_type: type) -> None:
    """Every MATLAB extractor must restore cwd after a timeout, with chained diagnostics."""
    previous = Path.cwd()
    asset = SimpleNamespace(
        ref_procfile_path="ref",
        dis_procfile_path="dis",
        ref_workfile_path="ref",
        dis_workfile_path="dis",
        quality_width_height=(576, 324),
        ref_yuv_type="yuv420p",
    )
    extractor = SimpleNamespace(
        MATLAB_WORKSPACE=str(tmp_path),
        logger=None,
        scale_list=[1],
        _get_log_file_path=mock.Mock(return_value=str(tmp_path / "log")),
        _get_workfile_yuv_type=mock.Mock(return_value="yuv420p"),
    )
    timeout = subprocess.TimeoutExpired(["matlab"], SCRIPT_TIMEOUT_S, output=b"partial\xff")
    with (
        mock.patch.object(
            matlab_feature_extractor.VmafExternalConfig,
            "get_and_assert_matlab",
            return_value="matlab",
        ),
        mock.patch.object(matlab_feature_extractor.subprocess, "run", side_effect=timeout),
    ):
        with pytest.raises(AssertionError, match="msg: partial�") as exc_info:
            extractor_type._generate_result(extractor, asset)
    assert exc_info.value.__cause__ is timeout
    assert Path.cwd() == previous


def test_mex_initialization_restores_cwd_on_timeout(tmp_path: Path) -> None:
    """MEX initialization shares exception-safe cwd restoration with feature runs."""
    previous = Path.cwd()
    timeout = subprocess.TimeoutExpired(["matlab"], SCRIPT_TIMEOUT_S)
    with (
        mock.patch.object(
            matlab_feature_extractor.VmafExternalConfig,
            "get_and_assert_matlab",
            return_value="matlab",
        ),
        mock.patch.object(matlab_feature_extractor.subprocess, "check_output", side_effect=timeout),
    ):
        with pytest.raises(AssertionError) as exc_info:
            matlab_feature_extractor.STMADFeatureExtractor._custom_init(
                SimpleNamespace(MATLAB_WORKSPACE=str(tmp_path))
            )
    assert exc_info.value.__cause__ is timeout
    assert Path.cwd() == previous
