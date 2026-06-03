# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance manifest tests for legacy corpus extraction scripts."""

from __future__ import annotations

import importlib.util
import json
import math
import sys
from pathlib import Path
from types import SimpleNamespace

import numpy as np
import pytest

pd = pytest.importorskip("pandas")

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_DIR = _REPO_ROOT / "ai" / "scripts"


def _load_module(name: str):
    path = _SCRIPT_DIR / f"{name}.py"
    spec = importlib.util.spec_from_file_location(f"{name}_manifest_test", path)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules[spec.name] = module
    spec.loader.exec_module(module)
    return module


def _fake_executable(path: Path) -> Path:
    path.write_text("#!/bin/sh\n", encoding="utf-8")
    path.chmod(0o755)
    return path


def test_extract_full_features_writes_manifest(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module("extract_full_features")
    data_root = tmp_path / "netflix"
    cache_dir = tmp_path / "cache"
    out = tmp_path / "full_features.parquet"
    vmaf_bin = _fake_executable(tmp_path / "vmaf")
    pair = SimpleNamespace(
        source="clip-a",
        ref_path=tmp_path / "ref.yuv",
        dis_path=tmp_path / "dis.yuv",
        width=16,
        height=16,
    )

    monkeypatch.setattr(mod, "iter_pairs", lambda _root, max_pairs=None: [pair])
    monkeypatch.setattr(
        mod,
        "extract_features",
        lambda *_args, **_kwargs: SimpleNamespace(
            feature_names=("vif_scale0", "adm2"),
            per_frame=np.asarray([[1.0, 2.0], [3.0, 4.0]], dtype=np.float32),
        ),
    )
    monkeypatch.setattr(
        mod,
        "teacher_scores",
        lambda *_args, **_kwargs: SimpleNamespace(
            per_frame=np.asarray([80.0, 81.0], dtype=np.float32)
        ),
    )

    rc = mod.main(
        [
            "--data-root",
            str(data_root),
            "--cache-dir",
            str(cache_dir),
            "--vmaf-bin",
            str(vmaf_bin),
            "--out",
            str(out),
            "--codec",
            "libx264",
        ]
    )

    manifest = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    frame = pd.read_parquet(out)
    assert rc == 0
    assert len(frame) == 2
    assert manifest["schema"] == "netflix-full-features-manifest-v1"
    assert manifest["stats"]["pairs"] == 1
    assert manifest["stats"]["rows"] == 2
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert manifest["run_provenance"]["args"]["codec"] == "libx264"


def test_bvi_dvc_jsonl_writes_manifest(tmp_path: Path) -> None:
    mod = _load_module("bvi_dvc_to_corpus_jsonl")
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir()
    (cache_dir / "ClipD_480x272_30fps_8bit_420.json").write_text(
        json.dumps(
            {
                "pooled_metrics": {"vmaf": {"mean": 71.5}, "adm2": {"mean": 0.93}},
                "frames": [{"metrics": {"vmaf": 71.5}}],
            }
        ),
        encoding="utf-8",
    )
    out = tmp_path / "bvi.jsonl"

    rc = mod.main(
        [
            "--cache-dir",
            str(cache_dir),
            "--output",
            str(out),
            "--encoder",
            "libsvtav1",
            "--preset",
            "8",
            "--crf",
            "31",
        ]
    )

    manifest = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    rows = [json.loads(line) for line in out.read_text(encoding="utf-8").splitlines()]
    assert rc == 0
    assert len(rows) == 1
    assert rows[0]["encoder"] == "libsvtav1"
    assert set(rows[0]) >= set(mod.CORPUS_ROW_KEYS)
    assert rows[0]["schema_version"] == mod.SCHEMA_VERSION
    assert rows[0]["adm2_mean"] == 0.93
    assert math.isnan(rows[0]["adm2_std"])
    assert manifest["schema"] == "bvi-dvc-corpus-jsonl-manifest-v1"
    assert manifest["stats"] == {"cache_files": 1, "rows": 1}
    assert manifest["run_provenance"]["args"]["preset"] == "8"


def test_konvid_vmaf_pairs_writes_manifest(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module("konvid_to_vmaf_pairs")
    root = tmp_path / "konvid-1k"
    videos = root / "KoNViD_1k_videos"
    videos.mkdir(parents=True)
    (videos / "100000.mp4").write_bytes(b"fake")
    vmaf_bin = _fake_executable(tmp_path / "vmaf")
    model = tmp_path / "model.json"
    model.write_text("{}", encoding="utf-8")
    out = tmp_path / "pairs.parquet"

    def fake_process_clip(*args, **_kwargs):
        key = args[0]
        return [
            {
                "key": key,
                "frame_index": 0,
                **{feature: 1.0 for feature in mod.DEFAULT_FEATURES},
                "vmaf": 88.0,
            }
        ]

    monkeypatch.setattr(mod, "_process_clip", fake_process_clip)

    rc = mod.main(
        [
            "--konvid-root",
            str(root),
            "--vmaf-bin",
            str(vmaf_bin),
            "--model",
            str(model),
            "--out",
            str(out),
            "--scratch",
            str(tmp_path / "scratch"),
            "--cache-dir",
            str(tmp_path / "cache"),
            "--crf",
            "37",
        ]
    )

    manifest = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    frame = pd.read_parquet(out)
    assert rc == 0
    assert len(frame) == 1
    assert manifest["schema"] == "konvid-vmaf-pairs-manifest-v1"
    assert manifest["stats"]["clips_selected"] == 1
    assert manifest["stats"]["clips_processed"] == 1
    assert manifest["stats"]["frames"] == 1
    assert manifest["crf"] == 37
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"
