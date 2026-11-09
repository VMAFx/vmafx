# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for predict ffmpeg execution timeout bounding."""

import subprocess

import pytest

from vmaftune import cli


def test_predict_ffmpeg_success(monkeypatch: pytest.MonkeyPatch) -> None:
    """Finished run with returncode 0 is returned unchanged and observed timeout bound."""
    calls: list[dict[str, object]] = []

    def stub_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Record call arguments and return a successful completed process."""
        calls.append({"args": args, "kwargs": kwargs})
        return subprocess.CompletedProcess(args=["ffmpeg"], returncode=0, stdout="ok", stderr="")

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(["ffmpeg", "-i", "in.yuv"], "reference extraction")
    assert result is not None
    assert result.returncode == 0
    assert result.stdout == "ok"
    assert len(calls) == 1
    assert calls[0]["kwargs"].get("timeout") == cli.FFMPEG_TIMEOUT_S


def test_predict_ffmpeg_timeout(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """TimeoutExpired returns None and emits bound warning message to stderr."""

    def stub_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Simulate an ffmpeg timeout by raising TimeoutExpired."""
        raise subprocess.TimeoutExpired(cmd=["ffmpeg"], timeout=cli.FFMPEG_TIMEOUT_S)

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(["ffmpeg", "-i", "in.yuv"], "reference extraction")
    assert result is None
    captured = capsys.readouterr()
    assert f"FFMPEG_TIMEOUT_S={cli.FFMPEG_TIMEOUT_S}" in captured.err
    assert "FFMPEG_TIMEOUT_S=60" in captured.err
    assert "reference extraction" in captured.err


def test_predict_ffmpeg_nonzero_exit(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Finished run with nonzero returncode is returned unchanged without stderr output."""

    def stub_run(*args: object, **kwargs: object) -> subprocess.CompletedProcess[str]:
        """Return a completed process with returncode 1 simulating failure."""
        return subprocess.CompletedProcess(
            args=["ffmpeg"], returncode=1, stdout="", stderr="ffmpeg failed"
        )

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(["ffmpeg", "-i", "in.yuv"], "decode")
    assert result is not None
    assert result.returncode == 1
    assert result.stderr == "ffmpeg failed"
    captured = capsys.readouterr()
    assert captured.err == ""
