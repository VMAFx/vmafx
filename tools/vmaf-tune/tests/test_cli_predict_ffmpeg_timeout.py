# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for predict ffmpeg execution timeout bounding."""

import argparse
import math
from collections.abc import Callable
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

# isort: split
import vmaftune.encode
import vmaftune.score
from vmaftune import cli
from vmaftune.per_shot import Shot

UHD_4K = (3840, 2160)
UHD_8K = (7680, 4320)
SD_360P = (640, 360)
FFMPEG_RATE = 0.04
SAMPLE_TIMEOUT_S = 72.0
CUSTOM_TIMEOUT_S = 123.4
BOUND_FLOOR_S = 60
DEFAULT_CRF = 23
TEST_FPS = 24.0
TEST_CODEC = "libx264"
TEST_PIX_FMT = "yuv420p"
PREDICT_VMAF_SCORE = 92.5
SHOT_START = 0
SHOT_END = 24


def test_predict_ffmpeg_success(monkeypatch: pytest.MonkeyPatch) -> None:
    """Finished run with returncode 0 is returned unchanged and observed timeout bound."""
    calls: list[dict[str, object]] = []

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        """Record call arguments and return a successful completed process."""
        calls.append({"args": args, "kwargs": kwargs})
        return cli.subprocess.CompletedProcess(
            args=["ffmpeg"], returncode=0, stdout="ok", stderr=""
        )

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(
        ["ffmpeg", "-i", "in.yuv"], "reference extraction", SAMPLE_TIMEOUT_S
    )
    assert result is not None
    assert result.returncode == 0
    assert result.stdout == "ok"
    assert len(calls) == 1
    assert calls[0]["kwargs"].get("timeout") == SAMPLE_TIMEOUT_S


def test_predict_ffmpeg_timeout(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """TimeoutExpired returns None and emits bound warning message to stderr."""

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        """Simulate an ffmpeg timeout by raising TimeoutExpired."""
        raise cli.subprocess.TimeoutExpired(cmd=["ffmpeg"], timeout=SAMPLE_TIMEOUT_S)

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(
        ["ffmpeg", "-i", "in.yuv"], "reference extraction", SAMPLE_TIMEOUT_S
    )
    assert result is None
    captured = capsys.readouterr()
    assert "vmaf-tune: ffmpeg reference extraction timed out after 72s" in captured.err
    assert "reference extraction" in captured.err


def test_predict_ffmpeg_nonzero_exit(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Finished run with nonzero returncode is returned unchanged without stderr output."""

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        """Return a completed process with returncode 1 simulating failure."""
        return cli.subprocess.CompletedProcess(
            args=["ffmpeg"], returncode=1, stdout="", stderr="ffmpeg failed"
        )

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    result = cli._run_predict_ffmpeg(["ffmpeg", "-i", "in.yuv"], "decode", SAMPLE_TIMEOUT_S)
    assert result is not None
    assert result.returncode == 1
    assert result.stderr == "ffmpeg failed"
    captured = capsys.readouterr()
    assert captured.err == ""


def test_run_predict_ffmpeg_passes_timeout(monkeypatch: pytest.MonkeyPatch) -> None:
    """_run_predict_ffmpeg passes timeout_s directly to subprocess.run."""
    calls: list[dict[str, object]] = []

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        calls.append({"args": args, "kwargs": kwargs})
        return cli.subprocess.CompletedProcess(
            args=["ffmpeg"], returncode=0, stdout="ok", stderr=""
        )

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    cli._run_predict_ffmpeg(["ffmpeg"], "reference extraction", CUSTOM_TIMEOUT_S)
    assert len(calls) == 1
    assert calls[0]["kwargs"].get("timeout") == CUSTOM_TIMEOUT_S


def test_frame_scaled_timeout_s_positive() -> None:
    """Positive helper cases at 4K resolution."""
    assert cli._frame_scaled_timeout_s(300, *UHD_4K, FFMPEG_RATE) == cli.BOUND_FLOOR_S + 12.0
    assert cli._frame_scaled_timeout_s(36000, *UHD_4K, FFMPEG_RATE) == cli.BOUND_FLOOR_S + 1440.0


def test_frame_scaled_timeout_s_boundary() -> None:
    """Boundary helper cases with clamping for sub-4K resolutions and <= 0 frames."""
    assert (
        round(cli._frame_scaled_timeout_s(1, *SD_360P, FFMPEG_RATE), 2) == cli.BOUND_FLOOR_S + 0.04
    )
    assert cli._frame_scaled_timeout_s(0, *SD_360P, FFMPEG_RATE) == float(cli.BOUND_FLOOR_S)
    assert cli._frame_scaled_timeout_s(-5, *SD_360P, FFMPEG_RATE) == float(cli.BOUND_FLOOR_S)


def test_frame_scaled_timeout_s_scaling() -> None:
    """Helper scaling for 8K resolution has 4 times the 4K area."""
    assert (
        cli._frame_scaled_timeout_s(300, *UHD_8K, FFMPEG_RATE)
        == cli.BOUND_FLOOR_S + 300 * 4 * FFMPEG_RATE
    )


def test_bound_floor_s_pins_sixty_second_minimum() -> None:
    """Assert BOUND_FLOOR_S contract floor is exactly 60 seconds."""
    assert cli.BOUND_FLOOR_S == BOUND_FLOOR_S


def test_extract_shot_to_raw_yuv_records_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_extract_shot_to_raw_yuv applies frame-scaled timeout bound above 60s."""
    calls: list[dict[str, object]] = []
    output = tmp_path / "out.yuv"

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        calls.append({"args": args, "kwargs": kwargs})
        output.write_bytes(b"")
        return cli.subprocess.CompletedProcess(args=["ffmpeg"], returncode=0, stdout="", stderr="")

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    args = argparse.Namespace(
        ffmpeg_bin="ffmpeg",
        src=tmp_path / "in.mp4",
        width=3840,
        height=2160,
        pix_fmt="yuv420p",
        framerate=60.0,
    )
    shot = Shot(start_frame=0, end_frame=36000)
    cli._extract_shot_to_raw_yuv(args, shot=shot, output=output)
    expected_timeout = cli._frame_scaled_timeout_s(36000, 3840, 2160, cli.FFMPEG_FRAME_TIMEOUT_S)
    assert len(calls) == 1
    assert calls[0]["kwargs"].get("timeout") == expected_timeout
    assert expected_timeout > BOUND_FLOOR_S


def test_extract_shot_to_raw_yuv_timeout_expired(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_extract_shot_to_raw_yuv raises RuntimeError naming bound and shot range on timeout."""
    output = tmp_path / "out.yuv"
    expected_timeout = cli._frame_scaled_timeout_s(36000, 3840, 2160, cli.FFMPEG_FRAME_TIMEOUT_S)

    def stub_run(*args: object, **kwargs: object) -> cli.subprocess.CompletedProcess[str]:
        raise cli.subprocess.TimeoutExpired(cmd=["ffmpeg"], timeout=expected_timeout)

    monkeypatch.setattr(cli.subprocess, "run", stub_run)
    args = argparse.Namespace(
        ffmpeg_bin="ffmpeg",
        src=tmp_path / "in.mp4",
        width=3840,
        height=2160,
        pix_fmt="yuv420p",
        framerate=60.0,
    )
    shot = Shot(start_frame=0, end_frame=36000)
    with pytest.raises(RuntimeError) as exc_info:
        cli._extract_shot_to_raw_yuv(args, shot=shot, output=output)
    assert f"{expected_timeout:.0f}s" in str(exc_info.value)
    assert "[0, 36000)" in str(exc_info.value)


def _setup_predict_scorer(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    decode_return: cli.subprocess.CompletedProcess[str] | None,
    touch_dist_yuv: bool = True,
) -> tuple[
    Callable[[Shot, int, str], tuple[Path, float]],
    list[object],
    Path,
    Path,
]:
    """Configure monkeypatches and return a real predict scorer with recorder."""
    score_requests: list[object] = []

    def fake_run_predict_ffmpeg(
        cmd: list[str], step: str, timeout_s: float
    ) -> cli.subprocess.CompletedProcess[str] | None:
        if step == "reference extraction":
            Path(cmd[-1]).touch()
            return cli.subprocess.CompletedProcess(args=cmd, returncode=0, stdout="", stderr="")
        if step == "decode":
            if touch_dist_yuv:
                Path(cmd[-1]).touch()
            return decode_return
        return None

    def fake_run_encode(req: Any, ffmpeg_bin: str = "ffmpeg") -> object:
        req.output.touch()
        return SimpleNamespace(exit_status=0)

    def fake_run_score(req: Any) -> object:
        score_requests.append(req)
        return SimpleNamespace(vmaf_score=PREDICT_VMAF_SCORE)

    monkeypatch.setattr(cli, "_run_predict_ffmpeg", fake_run_predict_ffmpeg)
    monkeypatch.setattr(vmaftune.encode, "run_encode", fake_run_encode)
    monkeypatch.setattr(vmaftune.score, "run_score", fake_run_score)

    args = argparse.Namespace(ffmpeg_bin="ffmpeg", source=tmp_path / "source.mp4")
    geometry = cli._PredictGeometry(
        width=SD_360P[0],
        height=SD_360P[1],
        fps=TEST_FPS,
        pix_fmt=TEST_PIX_FMT,
    )
    scorer = cli._build_real_predict_scorer(args, tmp_path, geometry)
    dist_path = tmp_path / f"dist_{SHOT_START}_{SHOT_END}.mp4"
    dist_yuv = tmp_path / f"dist_{SHOT_START}_{SHOT_END}.decoded.yuv"
    return scorer, score_requests, dist_path, dist_yuv


def test_real_encode_and_score_decode_success_scores_yuv(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Assert successful decode scores decoded YUV and returns measured score."""
    proc = cli.subprocess.CompletedProcess(args=["ffmpeg"], returncode=0, stdout="", stderr="")
    scorer, recorded, dist_path, dist_yuv = _setup_predict_scorer(
        monkeypatch, tmp_path, decode_return=proc, touch_dist_yuv=True
    )
    shot = Shot(start_frame=SHOT_START, end_frame=SHOT_END)
    out_path, score = scorer(shot, DEFAULT_CRF, TEST_CODEC)
    assert out_path == dist_path
    assert score == PREDICT_VMAF_SCORE
    assert len(recorded) == 1
    assert recorded[0].distorted == dist_yuv


def test_real_encode_and_score_decode_timeout_returns_nan(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Assert decode timeout returns NaN without running score."""
    scorer, recorded, dist_path, _ = _setup_predict_scorer(
        monkeypatch, tmp_path, decode_return=None, touch_dist_yuv=True
    )
    shot = Shot(start_frame=SHOT_START, end_frame=SHOT_END)
    out_path, score = scorer(shot, DEFAULT_CRF, TEST_CODEC)
    assert out_path == dist_path
    assert math.isnan(score)
    assert len(recorded) == 0


def test_real_encode_and_score_decode_failure_returns_nan(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Assert decode failure exit code returns NaN without running score."""
    proc = cli.subprocess.CompletedProcess(args=["ffmpeg"], returncode=1, stdout="", stderr="")
    scorer, recorded, dist_path, _ = _setup_predict_scorer(
        monkeypatch, tmp_path, decode_return=proc, touch_dist_yuv=True
    )
    shot = Shot(start_frame=SHOT_START, end_frame=SHOT_END)
    out_path, score = scorer(shot, DEFAULT_CRF, TEST_CODEC)
    assert out_path == dist_path
    assert math.isnan(score)
    assert len(recorded) == 0


def test_real_encode_and_score_missing_decoded_yuv_returns_nan(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Assert decode exit zero with missing output YUV returns NaN without running score."""
    proc = cli.subprocess.CompletedProcess(args=["ffmpeg"], returncode=0, stdout="", stderr="")
    scorer, recorded, dist_path, _ = _setup_predict_scorer(
        monkeypatch, tmp_path, decode_return=proc, touch_dist_yuv=False
    )
    shot = Shot(start_frame=SHOT_START, end_frame=SHOT_END)
    out_path, score = scorer(shot, DEFAULT_CRF, TEST_CODEC)
    assert out_path == dist_path
    assert math.isnan(score)
    assert len(recorded) == 0
