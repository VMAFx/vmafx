# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ``combine_full_feature_parquets.py``."""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pandas as pd
import pytest

from ai.data.feature_extractor import FULL_FEATURES

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT_PATH = _REPO_ROOT / "ai" / "scripts" / "combine_full_feature_parquets.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("combine_full_feature_parquets", _SCRIPT_PATH)
    assert spec is not None and spec.loader is not None
    module = importlib.util.module_from_spec(spec)
    sys.modules["combine_full_feature_parquets_test_import"] = module
    spec.loader.exec_module(module)
    return module


def _write_parquet(path: Path, rows: list[dict]) -> Path:
    pd.DataFrame(rows).to_parquet(path, index=False)
    return path


def test_normalise_shard_maps_key_to_source_and_fills_missing_features(tmp_path: Path) -> None:
    mod = _load_module()
    parquet = _write_parquet(
        tmp_path / "konvid.parquet",
        [
            {
                "key": "clip-a",
                "frame_index": 7,
                "teacher_model": "vmaf_v1.0.16_3d0h",
                "adm2": 0.9,
                "vmaf": 80.0,
            }
        ],
    )

    out = mod._normalise_shard("konvid", parquet)

    assert out.loc[0, "corpus"] == "konvid"
    assert out.loc[0, "source"] == "clip-a"
    assert out.loc[0, "frame_index"] == 7
    assert out.loc[0, "codec"] == "unknown"
    assert out.loc[0, "teacher_model"] == "vmaf_v1.0.16_3d0h"
    assert out.loc[0, "adm2"] == 0.9
    assert pd.isna(out.loc[0, "speed_temporal"])
    assert list(out.columns) == list(mod.OUTPUT_COLUMNS)


def test_normalise_shard_requires_vmaf(tmp_path: Path) -> None:
    mod = _load_module()
    parquet = _write_parquet(
        tmp_path / "broken.parquet",
        [{"source": "s", "teacher_model": "vmaf_v1.0.16_3d0h"}],
    )

    with pytest.raises(ValueError, match="missing required 'vmaf'"):
        mod._normalise_shard("broken", parquet)


def test_normalise_shard_refuses_legacy_without_assume_teacher(tmp_path: Path) -> None:
    mod = _load_module()
    parquet = _write_parquet(
        tmp_path / "legacy.parquet",
        [{"source": "s", "vmaf": 80.0}],
    )

    with pytest.raises(ValueError, match="missing required 'teacher_model'"):
        mod._normalise_shard("legacy", parquet)


def test_normalise_shard_accepts_legacy_with_assume_teacher(tmp_path: Path) -> None:
    mod = _load_module()
    parquet = _write_parquet(
        tmp_path / "legacy.parquet",
        [{"source": "s", "vmaf": 80.0}],
    )

    out = mod._normalise_shard("legacy", parquet, assume_teacher="vmaf_v0.6.1")
    assert out.loc[0, "teacher_model"] == "vmaf_v0.6.1"


def test_normalise_shard_refuses_mixed_teacher_within_shard(tmp_path: Path) -> None:
    mod = _load_module()
    parquet = _write_parquet(
        tmp_path / "mixed.parquet",
        [
            {"source": "s1", "vmaf": 80.0, "teacher_model": "vmaf_v1.0.16_3d0h"},
            {"source": "s2", "vmaf": 85.0, "teacher_model": "vmaf_v0.6.1"},
        ],
    )

    with pytest.raises(ValueError, match="mixed teacher_model values"):
        mod._normalise_shard("mixed", parquet)


def test_main_combines_multiple_labeled_inputs(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module()
    first = {
        "source": "netflix-src",
        "frame_index": 0,
        "codec": "unknown",
        "teacher_model": "vmaf_v1.0.16_3d0h",
        "vmaf": 70.0,
    }
    second = {
        "key": "bvi-src",
        "frame_index": 1,
        "codec": "x264",
        "teacher_model": "vmaf_v1.0.16_3d0h",
        "vmaf": 75.0,
    }
    for feature in FULL_FEATURES:
        first[feature] = 1.0
        second[feature] = 2.0
    p1 = _write_parquet(tmp_path / "netflix.parquet", [first])
    p2 = _write_parquet(tmp_path / "bvi.parquet", [second])
    out = tmp_path / "combined.parquet"

    monkeypatch.setattr(
        "sys.argv",
        [
            "combine_full_feature_parquets.py",
            "--input",
            f"netflix={p1}",
            "--input",
            f"bvi={p2}",
            "--out",
            str(out),
        ],
    )

    assert mod.main() == 0
    df = pd.read_parquet(out)
    assert df["corpus"].tolist() == ["netflix", "bvi"]
    assert df["source"].tolist() == ["netflix-src", "bvi-src"]
    assert df["codec"].tolist() == ["unknown", "x264"]
    assert df["teacher_model"].tolist() == ["vmaf_v1.0.16_3d0h", "vmaf_v1.0.16_3d0h"]
    manifest = json.loads(out.with_suffix(".manifest.json").read_text(encoding="utf-8"))
    assert manifest["schema"] == "full-feature-parquet-combine-manifest-v1"
    assert manifest["stats"]["output_rows"] == 2
    assert manifest["stats"]["corpora"] == {"netflix": 1, "bvi": 1}
    assert manifest["teacher_model"] == "vmaf_v1.0.16_3d0h"
    assert manifest["inputs"][0]["label"] == "netflix"
    assert manifest["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_main_refuses_conflicting_teachers_across_shards(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module()
    p1 = _write_parquet(
        tmp_path / "shard1.parquet",
        [{"source": "s1", "vmaf": 80.0, "teacher_model": "vmaf_v1.0.16_3d0h"}],
    )
    p2 = _write_parquet(
        tmp_path / "shard2.parquet",
        [{"source": "s2", "vmaf": 85.0, "teacher_model": "vmaf_v0.6.1"}],
    )
    out = tmp_path / "combined.parquet"

    monkeypatch.setattr(
        "sys.argv",
        [
            "combine_full_feature_parquets.py",
            "--input",
            f"s1={p1}",
            "--input",
            f"s2={p2}",
            "--out",
            str(out),
        ],
    )

    with pytest.raises(ValueError, match="conflicting teacher models"):
        mod.main()


def test_main_accepts_legacy_with_assume_teacher_flag(tmp_path: Path, monkeypatch) -> None:
    mod = _load_module()
    p1 = _write_parquet(
        tmp_path / "shard1.parquet",
        [{"source": "s1", "vmaf": 80.0}],
    )
    p2 = _write_parquet(
        tmp_path / "shard2.parquet",
        [{"source": "s2", "vmaf": 85.0}],
    )
    out = tmp_path / "combined.parquet"

    monkeypatch.setattr(
        "sys.argv",
        [
            "combine_full_feature_parquets.py",
            "--input",
            f"s1={p1}",
            "--input",
            f"s2={p2}",
            "--assume-teacher",
            "vmaf_v0.6.1",
            "--out",
            str(out),
        ],
    )

    assert mod.main() == 0
    df = pd.read_parquet(out)
    assert df["teacher_model"].tolist() == ["vmaf_v0.6.1", "vmaf_v0.6.1"]
