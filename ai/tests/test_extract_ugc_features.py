# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the YouTube UGC full-feature extractor helpers."""

from __future__ import annotations

import argparse
import importlib.util
import json
import sys
import time
import types
from pathlib import Path
from typing import Any

import pytest

from ai.data.feature_extractor import FULL_FEATURES
from scripts.lib.safe_subprocess import CommandFailed, CommandTimedOut, TextCommandResult

EXPECTED_VMAF_SCORE = 90.0

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "extract_ugc_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("extract_ugc_features", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["extract_ugc_features_test_import"] = module
    spec.loader.exec_module(module)
    return module


def test_schema_columns_track_full_features() -> None:
    mod = _load_module()

    assert (*FULL_FEATURES, "teacher_model", "vmaf") == mod.SCHEMA_COLS


def test_frame_row_reads_full_features_and_speed_aliases() -> None:
    mod = _load_module()
    metrics = {feature: 1.5 for feature in FULL_FEATURES}
    metrics["Speed_temporal_feature_speed_temporal_score"] = 2.0
    metrics.pop("speed_temporal")
    metrics["vmaf"] = 88.0

    row = mod._frame_row(metrics)

    assert set(FULL_FEATURES).issubset(row)
    assert row["teacher_model"] == mod.DEFAULT_MODEL
    assert row["speed_temporal"] == 2.0
    assert row["vmaf"] == 88.0


def test_run_vmaf_command_requests_full_feature_extractors(monkeypatch, tmp_path: Path) -> None:
    mod = _load_module()
    captured: dict[str, list[str]] = {}
    out_json = {"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        out_path = Path(cmd[cmd.index("--output") + 1])
        out_path.write_text(__import__("json").dumps(out_json))
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(mod, "run_command", fake_run)

    frames = mod._run_vmaf(
        tmp_path / "vmaf",
        tmp_path / "ref.yuv",
        tmp_path / "dis.yuv",
        16,
        16,
        1,
        tmp_path / "model.json",
    )

    assert frames == out_json["frames"]
    cmd = captured["cmd"]
    assert "-m" in cmd
    assert "path=" + str(tmp_path / "model.json") in cmd
    assert cmd.count("--feature") >= 10
    assert "speed_temporal" in cmd
    assert "speed_chroma" in cmd


def test_ffprobe_executable_allowlist_and_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_ffprobe passes allowed_executables=('ffprobe',) and timeout_seconds=FFMPEG_TIMEOUT_S."""
    mod = _load_module()
    captured_kwargs: dict[str, Any] = {}
    expected_width = 640

    def fake_run(cmd, **kwargs):
        captured_kwargs.update(kwargs)
        return TextCommandResult(
            tuple(cmd),
            0,
            json.dumps({"streams": [{"width": expected_width, "height": 360}]}),
            "",
        )

    monkeypatch.setattr(mod, "run_command", fake_run)
    stream = mod._ffprobe(tmp_path / "video.mp4")
    assert stream["width"] == expected_width
    assert captured_kwargs.get("allowed_executables") == ("ffprobe",)
    assert captured_kwargs.get("timeout_seconds") == mod.FFMPEG_TIMEOUT_S


def test_ffmpeg_decode_to_yuv_executable_allowlist_and_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_decode_to_yuv runs ffmpeg with the ffmpeg allowlist and FFMPEG_TIMEOUT_S."""
    mod = _load_module()
    captured_kwargs: dict[str, Any] = {}

    def fake_run(cmd, **kwargs):
        captured_kwargs.update(kwargs)
        tmp_dest = Path(cmd[-1])
        tmp_dest.parent.mkdir(parents=True, exist_ok=True)
        tmp_dest.write_bytes(b"\x00" * 384)
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(mod, "run_command", fake_run)
    frames = mod._decode_to_yuv(tmp_path / "src.mp4", tmp_path / "dest.yuv", 16, 16, 1)
    assert frames == 1
    assert captured_kwargs.get("allowed_executables") == ("ffmpeg",)
    assert captured_kwargs.get("timeout_seconds") == mod.FFMPEG_TIMEOUT_S


def test_vmaf_executable_allowlist_and_timeout(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """_run_vmaf passes allowed_executables=(argv[0],) and timeout_seconds=VMAF_TIMEOUT_S."""
    mod = _load_module()
    distinct_timeout_s = 61
    monkeypatch.setattr(mod, "VMAF_TIMEOUT_S", distinct_timeout_s)
    captured_cmd: list[str] = []
    captured_kwargs: dict[str, Any] = {}
    out_json = {"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}

    def fake_run(cmd, **kwargs):
        captured_cmd.extend(cmd)
        captured_kwargs.update(kwargs)
        out_path = Path(cmd[cmd.index("--output") + 1])
        out_path.write_text(json.dumps(out_json))
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(mod, "run_command", fake_run)
    frames = mod._run_vmaf(
        tmp_path / "vmaf",
        tmp_path / "ref.yuv",
        tmp_path / "dis.yuv",
        16,
        16,
        1,
        tmp_path / "model.json",
    )
    assert frames == out_json["frames"]
    assert captured_kwargs.get("allowed_executables") == (captured_cmd[0],)
    assert captured_kwargs.get("timeout_seconds") == distinct_timeout_s
    assert captured_kwargs.get("timeout_seconds") == mod.VMAF_TIMEOUT_S


def _make_main_fixture(tmp_path: Path) -> dict[str, Path]:
    """Create the minimal on-disk fixtures main() validates before the loop."""
    orig = tmp_path / "clip0_orig.mp4"
    orig.write_bytes(b"fake")
    vmaf_bin = tmp_path / "vmaf"
    vmaf_bin.write_text("#!/bin/sh\n")
    model = tmp_path / "model.json"
    model.write_text("{}")
    manifest = tmp_path / "manifest.json"
    manifest.write_text(json.dumps({"clip0": {"orig": str(orig)}}))
    return {
        "manifest": manifest,
        "yuv_dir": tmp_path / "yuv",
        "vmaf_bin": vmaf_bin,
        "model": model,
        "out_parquet": tmp_path / "out.parquet",
    }


def _run_main(mod, fx: dict[str, Path]) -> int:
    return mod.main(
        [
            "--manifest",
            str(fx["manifest"]),
            "--yuv-dir",
            str(fx["yuv_dir"]),
            "--vmaf-bin",
            str(fx["vmaf_bin"]),
            "--model",
            str(fx["model"]),
            "--out-parquet",
            str(fx["out_parquet"]),
        ]
    )


def test_main_skips_clip_with_zero_height_instead_of_crashing(monkeypatch, tmp_path: Path) -> None:
    """R3-18: a zero ffprobe height must skip just that clip (no
    ZeroDivisionError escaping the loop and aborting the whole run).

    Before the fix the geometry arithmetic ran outside the per-clip try and
    ``target_w = (ow * target_h) // oh`` raised ZeroDivisionError, which is
    NOT one of the ``CommandFailed`` cases the loop handled —
    so it propagated out of main().  Now it is caught and the run returns the
    graceful "no rows extracted" exit code (2) instead of raising.
    """
    mod = _load_module()
    fx = _make_main_fixture(tmp_path)

    # ffprobe / ffmpeg "present" on PATH.
    monkeypatch.setattr(mod.shutil, "which", lambda _name: "/usr/bin/" + _name)
    # Degenerate stream: height 0 → old code raised ZeroDivisionError at target_w.
    monkeypatch.setattr(mod, "_ffprobe", lambda _p: {"width": 640, "height": 0})

    # Must return gracefully (exit 2 == no rows) rather than raise.
    rc = _run_main(mod, fx)
    assert rc == 2  # graceful "no rows extracted", not an unhandled crash


def test_main_skips_clip_with_missing_height_key(monkeypatch, tmp_path: Path) -> None:
    """R3-18: an ffprobe stream missing 'height' must skip the clip (KeyError
    no longer escapes the per-clip guard)."""
    mod = _load_module()
    fx = _make_main_fixture(tmp_path)

    monkeypatch.setattr(mod.shutil, "which", lambda _name: "/usr/bin/" + _name)
    monkeypatch.setattr(mod, "_ffprobe", lambda _p: {"width": 640})  # no 'height'

    rc = _run_main(mod, fx)
    assert rc == 2  # graceful skip, not an unhandled KeyError


def test_run_vmaf_rejects_invalid_dimensions_and_threads(tmp_path: Path) -> None:
    mod = _load_module()
    vmaf = tmp_path / "vmaf"
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    model = tmp_path / "model.json"

    with pytest.raises(ValueError, match="w must be a positive integer"):
        mod._run_vmaf(vmaf, ref, dis, 0, 1080, 1, model)
    with pytest.raises(ValueError, match="w must be a positive integer"):
        mod._run_vmaf(vmaf, ref, dis, -1920, 1080, 1, model)
    with pytest.raises(ValueError, match="h must be a positive integer"):
        mod._run_vmaf(vmaf, ref, dis, 1920, 0, 1, model)
    with pytest.raises(ValueError, match="h must be a positive integer"):
        mod._run_vmaf(vmaf, ref, dis, 1920, -1080, 1, model)
    with pytest.raises(ValueError, match="n_threads must be a positive integer"):
        mod._run_vmaf(vmaf, ref, dis, 1920, 1080, 0, model)


def test_run_vmaf_rejects_empty_or_null_paths(tmp_path: Path) -> None:
    mod = _load_module()
    vmaf = tmp_path / "vmaf"
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    model = tmp_path / "model.json"

    with pytest.raises(ValueError, match="cannot be empty"):
        mod._run_vmaf(Path(""), ref, dis, 1920, 1080, 1, model)
    with pytest.raises(ValueError, match="cannot contain null bytes"):
        mod._run_vmaf(Path("vmaf\0bad"), ref, dis, 1920, 1080, 1, model)
    with pytest.raises(ValueError, match="cannot contain null bytes"):
        mod._run_vmaf(vmaf, Path("ref\0bad.yuv"), dis, 1920, 1080, 1, model)
    with pytest.raises(ValueError, match="cannot contain null bytes"):
        mod._run_vmaf(vmaf, ref, Path("dis\0bad.yuv"), 1920, 1080, 1, model)
    with pytest.raises(ValueError, match="cannot contain null bytes"):
        mod._run_vmaf(vmaf, ref, dis, 1920, 1080, 1, Path("model\0bad.json"))


def test_run_vmaf_validates_scratch_dir(monkeypatch, tmp_path: Path) -> None:
    mod = _load_module()
    vmaf = tmp_path / "vmaf"
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    model = tmp_path / "model.json"

    monkeypatch.setenv("VMAF_TINY_AI_SCRATCH", "   ")
    with pytest.raises(ValueError, match="VMAF_TINY_AI_SCRATCH cannot be empty"):
        mod._run_vmaf(vmaf, ref, dis, 1920, 1080, 1, model)

    monkeypatch.setenv("VMAF_TINY_AI_SCRATCH", "relative/scratch/dir")
    with pytest.raises(ValueError, match="VMAF_TINY_AI_SCRATCH must be an absolute path"):
        mod._run_vmaf(vmaf, ref, dis, 1920, 1080, 1, model)


def test_run_vmaf_honours_valid_scratch_dir(monkeypatch, tmp_path: Path) -> None:
    mod = _load_module()
    scratch = tmp_path / "custom_scratch"
    monkeypatch.setenv("VMAF_TINY_AI_SCRATCH", str(scratch))

    out_json = {"frames": [{"frameNum": 0, "metrics": {"vmaf": 90.0}}]}
    captured: dict[str, list[str]] = {}

    def fake_run(cmd, **kwargs):
        captured["cmd"] = cmd
        out_path = Path(cmd[cmd.index("--output") + 1])
        assert scratch.resolve() in out_path.parents or out_path.parent == scratch.resolve()
        out_path.write_text(__import__("json").dumps(out_json))
        return TextCommandResult(tuple(cmd), 0, "", "")

    monkeypatch.setattr(mod, "run_command", fake_run)

    frames = mod._run_vmaf(
        tmp_path / "vmaf",
        tmp_path / "ref.yuv",
        tmp_path / "dis.yuv",
        1920,
        1080,
        1,
        tmp_path / "model.json",
    )
    assert frames == out_json["frames"]


_TEACHER = types.SimpleNamespace(arg="teacher.model", name="test_teacher")


def _ugc_args(tmp_path: Path) -> argparse.Namespace:
    """Return mock CLI arguments for UGC feature extraction."""
    return argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )


def _run_distorted(mod: types.ModuleType, tmp_path: Path) -> tuple[list[dict[str, object]], bool]:
    """Run distorted clip processing against mock inputs."""
    result: tuple[list[dict[str, object]], bool] = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        _ugc_args(tmp_path),
        _TEACHER,
        time.monotonic(),
        0,
    )
    return result


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "exc",
    [
        CommandTimedOut(("ffmpeg",), 60, None, None),
        CommandFailed(TextCommandResult(("ffmpeg",), 1, "", "")),
    ],
    ids=["timeout", "exit"],
)
def test_process_distorted_clip_decode_dis(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    tmp_path: Path,
    exc: Exception,
) -> None:
    """Verify that decode-dis failure logs an error and skips the clip."""
    mod = _load_module()

    def fake_decode(*_args: object, **_kwargs: object) -> None:
        """Simulate decode-dis failure by raising the given exception."""
        raise exc

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    rows, ok = _run_distorted(mod, tmp_path)
    assert (rows, ok) == ([], False)
    captured = capsys.readouterr().out
    assert "decode-dis failed" in captured


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "exc",
    [
        CommandTimedOut(("vmaf",), 60, None, None),
        CommandFailed(TextCommandResult(("vmaf",), 1, "", "")),
    ],
    ids=["timeout", "exit"],
)
def test_process_distorted_clip_vmaf(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    tmp_path: Path,
    exc: Exception,
) -> None:
    """Verify that vmaf failure logs an error, unlinks YUV, and skips the clip."""
    mod = _load_module()

    def fake_decode(_src: object, dst: Path, *_args: object, **_kwargs: object) -> None:
        """Write placeholder YUV content for distorted clip."""
        dst.write_bytes(b"dis_yuv")

    def fake_run_vmaf(*_args: object, **_kwargs: object) -> None:
        """Simulate vmaf failure by raising the given exception."""
        raise exc

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(mod, "_run_vmaf", fake_run_vmaf)
    rows, ok = _run_distorted(mod, tmp_path)
    assert (rows, ok) == ([], False)
    captured = capsys.readouterr().out
    assert "vmaf failed" in captured
    dis_yuv = tmp_path / "clip0_dis_640x360.yuv"
    assert not dis_yuv.exists()


def test_process_distorted_clip_vmaf_success(
    monkeypatch: pytest.MonkeyPatch, tmp_path: Path
) -> None:
    """Verify that successful vmaf execution returns extracted rows."""
    mod = _load_module()

    monkeypatch.setattr(mod, "_decode_to_yuv", lambda *_args, **_kwargs: None)
    monkeypatch.setattr(
        mod,
        "_run_vmaf",
        lambda *_args, **_kwargs: [{"frameNum": 0, "metrics": {"vmaf": EXPECTED_VMAF_SCORE}}],
    )
    rows, ok = _run_distorted(mod, tmp_path)
    assert ok is True
    assert len(rows) == 1
    assert rows[0]["corpus"] == "ugc"
    assert rows[0]["source"] == "ugc-clip0-dis"
    assert rows[0]["frame_index"] == 0
    assert rows[0]["teacher_model"] == "test_teacher"
    assert rows[0]["vmaf"] == EXPECTED_VMAF_SCORE


@pytest.mark.parametrize(  # type: ignore[untyped-decorator]  # pytest is untyped for the mypy gate (--no-site-packages)
    "exc",
    [
        CommandTimedOut(("ffmpeg",), 60, None, None),
        CommandFailed(TextCommandResult(("ffmpeg",), 1, "", "")),
    ],
    ids=["timeout", "exit"],
)
def test_decode_orig(
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
    tmp_path: Path,
    exc: Exception,
) -> None:
    """Verify that decode-orig failure logs an error and returns None."""
    mod = _load_module()

    monkeypatch.setattr(mod, "_ffprobe", lambda _p: {"width": 640, "height": 360})

    def fake_decode(*_args: object, **_kwargs: object) -> None:
        """Simulate decode-orig failure by raising the given exception."""
        raise exc

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    res = mod._decode_orig("clip0", tmp_path / "orig.mp4", _ugc_args(tmp_path))
    assert res is None
    captured = capsys.readouterr().out
    assert "decode-orig failed" in captured
