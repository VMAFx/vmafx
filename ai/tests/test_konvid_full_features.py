# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``ai/scripts/konvid_to_full_features.py``.

The real script runs ffmpeg + libvmaf over KoNViD-1k. These tests keep
the contract cheap by mocking subprocess calls and asserting the output
schema / fold file shape.
"""

from __future__ import annotations

import importlib.util
import json
import shlex
import sys
from pathlib import Path
from typing import Any
from unittest.mock import patch

import pandas as pd
import pytest

from ai.data.feature_extractor import FULL_FEATURES
from scripts.lib.safe_subprocess import (
    CommandFailed,
    CommandTimedOut,
    CommandValidationError,
    TextCommandResult,
)

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "konvid_to_full_features.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("konvid_to_full_features", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["konvid_to_full_features_test_import"] = module
    spec.loader.exec_module(module)
    return module


def _make_fake_vmaf_json(n_frames: int = 2) -> str:
    frames = []
    for frame_num in range(n_frames):
        metrics = {feature: 1.0 + frame_num for feature in FULL_FEATURES}
        metrics["vmaf"] = 80.0 - frame_num
        frames.append({"frameNum": frame_num, "metrics": metrics})
    return json.dumps({"frames": frames})


def _mock_run_command(cmd, **kwargs):
    """Fake successful commands with stream metadata or a written scoring JSON fixture."""
    stdout = ""
    if cmd and cmd[0] == "ffprobe":
        stdout = json.dumps({"streams": [{"width": 960, "height": 540, "codec_name": "h264"}]})

    if cmd and "--output" in cmd:
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(_make_fake_vmaf_json())

    return TextCommandResult(tuple(cmd), 0, stdout, "")


def test_resolve_videos_dir_accepts_nested_konvid_layout(tmp_path: Path) -> None:
    mod = _load_module()
    videos = tmp_path / "KoNViD_1k_videos"
    videos.mkdir()
    (videos / "10008004183.mp4").write_bytes(b"fake")

    assert mod._resolve_videos_dir(tmp_path) == videos


def test_assign_folds_is_deterministic_and_balanced() -> None:
    mod = _load_module()
    keys = [f"clip-{idx:03d}" for idx in range(20)]

    first = mod._assign_folds(keys, 5)
    second = mod._assign_folds(list(reversed(keys)), 5)

    assert first == second
    counts = {fold: list(first.values()).count(fold) for fold in set(first.values())}
    assert counts == {"fold0": 4, "fold1": 4, "fold2": 4, "fold3": 4, "fold4": 4}


def test_process_clip_cache_uses_strict_json(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module()
    cache_dir = tmp_path / "cache"
    scratch = tmp_path / "scratch"
    scratch.mkdir()
    monkeypatch.setattr(mod, "_decode_yuv", lambda *_args, **_kwargs: (16, 16, "h264"))
    monkeypatch.setattr(mod, "_encode_dis", lambda *_args, **_kwargs: None)

    def fake_run_vmaf(*args, **_kwargs) -> None:
        args[5].write_text(
            json.dumps(
                {
                    "frames": [
                        {
                            "frameNum": 0,
                            "metrics": {
                                "vmaf": float("nan"),
                                "adm2": float("inf"),
                                "motion2": float("-inf"),
                            },
                        }
                    ]
                }
            ),
            encoding="utf-8",
        )

    monkeypatch.setattr(mod, "_run_vmaf_full", fake_run_vmaf)
    mod._process_clip(
        "clip-a",
        tmp_path / "source.mp4",
        tmp_path / "vmaf",
        "version=vmaf_v0.6.1",
        "vmaf_v0.6.1",
        35,
        cache_dir,
        scratch,
        "h264",
        False,
    )

    cache = mod._cache_path(cache_dir, "clip-a", 35, "vmaf_v0.6.1")
    raw = cache.read_text(encoding="utf-8")
    assert "NaN" not in raw
    assert "Infinity" not in raw
    metrics = json.loads(raw)["frames"][0]["metrics"]
    assert metrics == {"adm2": None, "motion2": None, "vmaf": None}


def _setup_full_features_run(tmp_path: Path, monkeypatch) -> tuple[Path, Path]:
    """Plant a two-clip corpus and command-line arguments for ``main``."""
    root = tmp_path / "konvid-1k"
    videos = root / "KoNViD_1k_videos"
    videos.mkdir(parents=True)
    for name in ("10008004183.mp4", "10013374164.mp4"):
        (videos / name).write_bytes(b"fake mp4")

    fake_vmaf = tmp_path / "vmaf"
    fake_vmaf.write_bytes(b"")
    fake_vmaf.chmod(0o755)
    fake_model = tmp_path / "model.json"
    fake_model.write_text("{}")
    out_plain = tmp_path / "full_features_konvid.parquet"
    out_folds = tmp_path / "full_features_konvid_with_folds.parquet"

    monkeypatch.setattr(
        "sys.argv",
        [
            "konvid_to_full_features.py",
            "--konvid-root",
            str(root),
            "--vmaf-bin",
            str(fake_vmaf),
            "--model",
            str(fake_model),
            "--out",
            str(out_plain),
            "--folds-out",
            str(out_folds),
            "--scratch",
            str(tmp_path / "scratch"),
            "--cache-dir",
            str(tmp_path / "cache"),
            "--max-clips",
            "2",
        ],
    )
    return out_plain, out_folds


def test_main_writes_full_and_folded_parquets(tmp_path: Path, monkeypatch) -> None:
    """Verify extraction writes full and folded parquet tables with provenance metadata."""
    mod = _load_module()
    out_plain, out_folds = _setup_full_features_run(tmp_path, monkeypatch)

    with patch.object(mod, "run_command", side_effect=_mock_run_command):
        rc = mod.main()

    assert rc == 0
    plain = pd.read_parquet(out_plain)
    folded = pd.read_parquet(out_folds)
    assert len(plain) == 4
    assert len(folded) == 4
    for feature in FULL_FEATURES:
        assert feature in plain.columns
    assert "codec" in plain.columns
    assert "teacher_model" in plain.columns
    assert (plain["teacher_model"] == "model").all()
    assert "source" not in plain.columns
    assert set(folded["source"]).issubset({"fold0", "fold1", "fold2", "fold3", "fold4"})

    manifest = json.loads(out_plain.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == "konvid-full-features-manifest-v1"
    assert manifest["teacher_model"] == "model"
    assert manifest["features"] == list(FULL_FEATURES)
    assert manifest["folds"] == {"enabled": True, "fold_count": 5}
    assert manifest["stats"]["clips_selected"] == 2
    assert manifest["stats"]["clips_processed"] == 2
    assert manifest["stats"]["frames"] == 4
    assert manifest["stats"]["columns"] == len(plain.columns)
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert manifest["run_provenance"]["args"]["max_clips"] == 2


def test_run_command_bounds_and_allowlist(tmp_path: Path) -> None:
    """Positive test: verify run_command timeout_seconds and allowed_executables."""
    mod = _load_module()
    src = tmp_path / "clip.mp4"
    src.write_bytes(b"\x00")
    ref = tmp_path / "ref.yuv"
    dis = tmp_path / "dis.yuv"
    out_json = tmp_path / "vmaf.json"
    binary = tmp_path / "vmaf"
    binary.write_text("")

    recorded: list[dict[str, Any]] = []

    def fake_run(cmd, *, allowed_executables, timeout_seconds, **kw):
        """Record command constraints and fake metadata or empty successful command output."""
        recorded.append(
            {
                "cmd": list(cmd),
                "allowed": allowed_executables,
                "timeout": timeout_seconds,
            }
        )
        if cmd[0] == "ffprobe":
            return TextCommandResult(
                tuple(cmd),
                0,
                json.dumps({"streams": [{"width": 960, "height": 540, "codec_name": "h264"}]}),
                "",
            )
        return TextCommandResult(tuple(cmd), 0, "", "")

    with patch.object(mod, "run_command", side_effect=fake_run):
        mod._decode_yuv(src, ref)
        mod._encode_dis(src, dis, crf=28)
        mod._run_vmaf_full(binary, ref, dis, 960, 540, out_json, "version=vmaf_v0.6.1")

    expected_calls = 5
    assert len(recorded) == expected_calls
    # ffprobe in _decode_yuv
    assert recorded[0]["allowed"] == ("ffprobe",)
    assert recorded[0]["timeout"] == mod.FFMPEG_TIMEOUT_S
    # ffmpeg decode
    assert recorded[1]["allowed"] == ("ffmpeg",)
    assert recorded[1]["timeout"] == mod.FFMPEG_TIMEOUT_S
    # ffmpeg encode
    assert recorded[2]["allowed"] == ("ffmpeg",)
    assert recorded[2]["timeout"] == mod.FFMPEG_TIMEOUT_S
    assert recorded[3]["allowed"] == ("ffmpeg",)
    assert recorded[3]["timeout"] == mod.FFMPEG_TIMEOUT_S
    # vmaf
    vmaf_arg = mod.executable_argument(binary, Path.cwd())
    assert recorded[4]["allowed"] == (vmaf_arg,)
    assert recorded[4]["timeout"] == mod.VMAF_TIMEOUT_S


def test_vmaf_step_command_failed_skips_clip(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Negative test: CommandFailed from vmaf step logs FAILED and continues batch."""
    mod = _load_module()
    _out_plain, _out_folds = _setup_full_features_run(tmp_path, monkeypatch)

    def fake_run(cmd, **kw):
        """Fake metadata success, raise on scoring, and return success for other commands."""
        if cmd[0] == "ffprobe":
            return TextCommandResult(
                tuple(cmd),
                0,
                json.dumps({"streams": [{"width": 960, "height": 540, "codec_name": "h264"}]}),
                "",
            )
        if "--distorted" in cmd:
            raise CommandFailed(TextCommandResult(tuple(cmd), 1, "", "vmaf failed"))
        return TextCommandResult(tuple(cmd), 0, "", "")

    with patch.object(mod, "run_command", side_effect=fake_run):
        rc = mod.main()

    assert rc == 0
    captured = capsys.readouterr().err
    assert "[konvid-full]" in captured
    assert "FAILED:" in captured


def test_encode_step_command_timed_out_skips_clip_and_cleans_intermediate(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, capsys: pytest.CaptureFixture[str]
) -> None:
    """Boundary test: CommandTimedOut from encode step cleans up .dis.mp4 and batch continues."""
    mod = _load_module()
    _out_plain, _out_folds = _setup_full_features_run(tmp_path, monkeypatch)

    intermediate_paths: list[Path] = []
    timed_out_cmd: list[list[str]] = []

    def fake_run(cmd, **kw):
        """Fake metadata success or write a partial encoded file and raise CommandTimedOut."""
        if cmd[0] == "ffprobe":
            return TextCommandResult(
                tuple(cmd),
                0,
                json.dumps({"streams": [{"width": 960, "height": 540, "codec_name": "h264"}]}),
                "",
            )
        if "-c:v" in cmd:
            # First encode step: plant intermediate file and simulate timeout
            timed_out_cmd.append(list(cmd))
            inter = Path(cmd[-1])
            intermediate_paths.append(inter)
            inter.parent.mkdir(parents=True, exist_ok=True)
            inter.write_bytes(b"partial intermediate")
            raise CommandTimedOut(tuple(cmd), 60.0, None, None)
        return TextCommandResult(tuple(cmd), 0, "", "")

    with patch.object(mod, "run_command", side_effect=fake_run):
        rc = mod.main()

    assert rc == 0
    captured = capsys.readouterr().err
    assert "[konvid-full]" in captured
    assert "timed out after 60s" in captured
    assert len(timed_out_cmd) > 0
    expected_stderr = (
        f"[konvid-full] 10008004183 FAILED: {shlex.join(timed_out_cmd[0])} (timed out after 60s)"
    )
    assert expected_stderr in captured.splitlines()
    assert len(intermediate_paths) > 0
    for inter in intermediate_paths:
        assert not inter.exists(), f"Intermediate {inter} was not unlinked in finally"


def test_run_rejects_unallowed_executable() -> None:
    """Invoking _run with an executable not in the allowlist raises CommandValidationError."""
    mod = _load_module()
    with pytest.raises(CommandValidationError):
        mod._run(["sh", "-c", "true"], allowed=("ffmpeg",), timeout_s=1)
