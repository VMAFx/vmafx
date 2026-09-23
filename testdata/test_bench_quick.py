#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2

"""Behavioral tests for the bounded quick benchmark harness."""

from __future__ import annotations

import subprocess
from pathlib import Path

import pytest

from testdata import bench_quick


def _fixture_pair(directory: Path, dimensions: str = "576x324") -> None:
    (directory / f"ref_{dimensions}_48f.yuv").touch()
    (directory / f"dis_{dimensions}_48f.yuv").touch()


def test_main_returns_two_when_no_complete_pair(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    monkeypatch.setenv("VMAF_TESTDATA", str(tmp_path))
    monkeypatch.setattr(bench_quick, "RESOLUTIONS", ("576x324",))
    (tmp_path / "ref_576x324_48f.yuv").touch()

    assert bench_quick.main() == 2
    assert "No complete 48-frame fixture pairs" in capsys.readouterr().err


def test_main_propagates_benchmark_failure(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _fixture_pair(tmp_path)
    monkeypatch.setenv("VMAF_TESTDATA", str(tmp_path))
    monkeypatch.setattr(bench_quick, "RESOLUTIONS", ("576x324",))
    monkeypatch.setattr(
        bench_quick,
        "run_command",
        lambda *_args, **_kwargs: subprocess.CompletedProcess([], 9, "", "fixture failed\n"),
    )

    assert bench_quick.main() == 1
    assert "576x324: FAILED: fixture failed" in capsys.readouterr().err


def test_main_reports_three_successful_rates(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    _fixture_pair(tmp_path)
    monkeypatch.setenv("VMAF_TESTDATA", str(tmp_path))
    monkeypatch.setattr(bench_quick, "RESOLUTIONS", ("576x324",))
    monkeypatch.setattr(
        bench_quick,
        "run_command",
        lambda *_args, **_kwargs: subprocess.CompletedProcess([], 0, "", ""),
    )
    ticks = iter((0.0, 1.0, 2.0, 4.0, 5.0, 9.0))
    monkeypatch.setattr(bench_quick, "_monotonic_seconds", lambda: next(ticks))

    assert bench_quick.main() == 0
    output = capsys.readouterr().out
    assert "576x324: best=48.0 avg=28.0 fps" in output
    assert "(48.0, 24.0, 12.0)" in output
