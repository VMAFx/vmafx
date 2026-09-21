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
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock, patch

import pandas as pd
import pytest

from ai.data.feature_extractor import FULL_FEATURES

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "konvid_to_full_features.py"


def _load_module() -> Any:
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


def _mock_subprocess_run(cmd: Any, **kwargs: Any) -> Any:
    result = MagicMock()
    result.returncode = 0
    result.stderr = ""
    result.stdout = ""

    if cmd and cmd[0] == "ffprobe":
        result.stdout = json.dumps(
            {"streams": [{"width": 960, "height": 540, "codec_name": "h264"}]}
        )
        return result

    if cmd and "--output" in cmd:
        out_idx = cmd.index("--output") + 1
        out_path = Path(cmd[out_idx])
        out_path.parent.mkdir(parents=True, exist_ok=True)
        out_path.write_text(_make_fake_vmaf_json())

    return result


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
    counts = {fold: list[Any](first.values()).count(fold) for fold in set(first.values())}
    assert counts == {"fold0": 4, "fold1": 4, "fold2": 4, "fold3": 4, "fold4": 4}


@dataclass(frozen=True)
class _KonvidFixture:
    """Paths of the synthetic KoNViD corpus and the CLI outputs under test."""

    root: Path
    vmaf_bin: Path
    model: Path
    out_plain: Path
    out_folds: Path


def _prepare_konvid_fixture(tmp_path: Path) -> _KonvidFixture:
    """Create a two-clip KoNViD tree plus a stub vmaf binary and model JSON."""
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

    return _KonvidFixture(
        root=root,
        vmaf_bin=fake_vmaf,
        model=fake_model,
        out_plain=tmp_path / "full_features_konvid.parquet",
        out_folds=tmp_path / "full_features_konvid_with_folds.parquet",
    )


def _assert_konvid_manifest(manifest: dict[str, Any], plain: pd.DataFrame) -> None:
    """Assert the replay manifest the script writes beside the plain parquet."""
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


def test_main_writes_full_and_folded_parquets(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    mod = _load_module()
    fixture = _prepare_konvid_fixture(tmp_path)

    monkeypatch.setattr(
        "sys.argv",
        [
            "konvid_to_full_features.py",
            "--konvid-root",
            str(fixture.root),
            "--vmaf-bin",
            str(fixture.vmaf_bin),
            "--model",
            str(fixture.model),
            "--out",
            str(fixture.out_plain),
            "--folds-out",
            str(fixture.out_folds),
            "--scratch",
            str(tmp_path / "scratch"),
            "--cache-dir",
            str(tmp_path / "cache"),
            "--max-clips",
            "2",
        ],
    )

    with patch("subprocess.run", side_effect=_mock_subprocess_run):
        rc = mod.main()

    assert rc == 0
    plain = pd.read_parquet(fixture.out_plain)
    folded = pd.read_parquet(fixture.out_folds)
    assert len(plain) == 4
    assert len(folded) == 4
    for feature in FULL_FEATURES:
        assert feature in plain.columns
    assert "codec" in plain.columns
    assert "teacher_model" in plain.columns
    assert (plain["teacher_model"] == "model").all()
    assert "source" not in plain.columns
    assert set(folded["source"]).issubset({"fold0", "fold1", "fold2", "fold3", "fold4"})

    manifest = json.loads(
        fixture.out_plain.with_suffix(".manifest.json").read_text(encoding="utf-8")
    )
    _assert_konvid_manifest(manifest, plain)
