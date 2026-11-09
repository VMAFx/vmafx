# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for the YouTube UGC full-feature extractor helpers."""

from __future__ import annotations

import argparse
import importlib.util
import json
import subprocess
import sys
import time
import types
from pathlib import Path

import pytest

from ai.data.feature_extractor import FULL_FEATURES

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
        return None

    monkeypatch.setattr(mod.subprocess, "run", fake_run)

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
    NOT one of the ``subprocess.CalledProcessError`` cases the loop handled —
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
        return None

    monkeypatch.setattr(mod.subprocess, "run", fake_run)

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


def test_process_distorted_clip_decode_dis_timeout(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that decode-dis timeout logs a failure and skips the clip."""
    mod = _load_module()

    def fake_decode(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["ffmpeg"], 60)

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    teacher = types.SimpleNamespace(arg="teacher.model", name="test_teacher")
    rows, ok = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        args,
        teacher,
        time.monotonic(),
        0,
    )
    assert (rows, ok) == ([], False)
    captured = capsys.readouterr().out
    assert "decode-dis failed" in captured


def test_process_distorted_clip_decode_dis_called_process_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that decode-dis process error logs a failure and skips the clip."""
    mod = _load_module()

    def fake_decode(*_args, **_kwargs):
        raise subprocess.CalledProcessError(1, ["ffmpeg"])

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    teacher = types.SimpleNamespace(arg="teacher.model", name="test_teacher")
    rows, ok = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        args,
        teacher,
        time.monotonic(),
        0,
    )
    assert (rows, ok) == ([], False)
    captured = capsys.readouterr().out
    assert "decode-dis failed" in captured


def test_process_distorted_clip_vmaf_timeout(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that vmaf timeout logs a failure, unlinks YUV, and skips the clip."""
    mod = _load_module()

    def fake_decode(_src, dst, *_args, **_kwargs):
        dst.write_bytes(b"dis_yuv")

    def fake_run_vmaf(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["ffmpeg"], 60)

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(mod, "_run_vmaf", fake_run_vmaf)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    teacher = types.SimpleNamespace(arg="teacher.model", name="test_teacher")
    rows, ok = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        args,
        teacher,
        time.monotonic(),
        0,
    )
    assert (rows, ok) == ([], False)
    captured = capsys.readouterr().out
    assert "vmaf failed" in captured
    dis_yuv = tmp_path / "clip0_dis_640x360.yuv"
    assert not dis_yuv.exists()


def test_process_distorted_clip_vmaf_called_process_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that vmaf process error logs a failure, unlinks YUV, and skips the clip."""
    mod = _load_module()

    def fake_decode(_src, dst, *_args, **_kwargs):
        dst.write_bytes(b"dis_yuv")

    def fake_run_vmaf(*_args, **_kwargs):
        raise subprocess.CalledProcessError(1, ["ffmpeg"])

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    monkeypatch.setattr(mod, "_run_vmaf", fake_run_vmaf)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    teacher = types.SimpleNamespace(arg="teacher.model", name="test_teacher")
    rows, ok = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        args,
        teacher,
        time.monotonic(),
        0,
    )
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
        lambda *_args, **_kwargs: [{"frameNum": 0, "metrics": {"vmaf": 90.0}}],
    )
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    teacher = types.SimpleNamespace(arg="teacher.model", name="test_teacher")
    rows, ok = mod._process_distorted_clip(
        "clip0",
        "dis",
        tmp_path / "dis.mp4",
        tmp_path / "ref.yuv",
        640,
        360,
        args,
        teacher,
        time.monotonic(),
        0,
    )
    assert ok is True
    assert len(rows) == 1
    assert rows[0]["corpus"] == "ugc"
    assert rows[0]["source"] == "ugc-clip0-dis"
    assert rows[0]["frame_index"] == 0
    assert rows[0]["teacher_model"] == "test_teacher"
    assert rows[0]["vmaf"] == 90.0


def test_decode_orig_timeout(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that decode-orig timeout logs a failure and returns None."""
    mod = _load_module()

    monkeypatch.setattr(mod, "_ffprobe", lambda _p: {"width": 640, "height": 360})

    def fake_decode(*_args, **_kwargs):
        raise subprocess.TimeoutExpired(["ffmpeg"], 60)

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    res = mod._decode_orig("clip0", tmp_path / "orig.mp4", args)
    assert res is None
    captured = capsys.readouterr().out
    assert "decode-orig failed" in captured


def test_decode_orig_called_process_error(
    monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str], tmp_path: Path
) -> None:
    """Verify that decode-orig process error logs a failure and returns None."""
    mod = _load_module()

    monkeypatch.setattr(mod, "_ffprobe", lambda _p: {"width": 640, "height": 360})

    def fake_decode(*_args, **_kwargs):
        raise subprocess.CalledProcessError(1, ["ffmpeg"])

    monkeypatch.setattr(mod, "_decode_to_yuv", fake_decode)
    args = argparse.Namespace(
        yuv_dir=tmp_path,
        max_frames=100,
        vmaf_bin=tmp_path / "vmaf",
        threads=1,
        keep_yuv=False,
        max_height=360,
    )
    res = mod._decode_orig("clip0", tmp_path / "orig.mp4", args)
    assert res is None
    captured = capsys.readouterr().out
    assert "decode-orig failed" in captured
