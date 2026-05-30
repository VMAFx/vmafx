# Copyright 2026 Lusoris and Claude (Anthropic)
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the YouTube UGC full-feature extractor helpers."""

from __future__ import annotations

from pathlib import Path

from ai.data.feature_extractor import FULL_FEATURES

from .conftest import load_ai_script


def _load_module():
    return load_ai_script("extract_ugc_features", name="extract_ugc_features_test_import")


def test_schema_columns_track_full_features() -> None:
    mod = _load_module()

    assert (*FULL_FEATURES, "vmaf") == mod.SCHEMA_COLS


def test_frame_row_reads_full_features_and_speed_aliases() -> None:
    mod = _load_module()
    metrics = {feature: 1.5 for feature in FULL_FEATURES}
    metrics["Speed_temporal_feature_speed_temporal_score"] = 2.0
    metrics.pop("speed_temporal")
    metrics["vmaf"] = 88.0

    row = mod._frame_row(metrics)

    assert set(FULL_FEATURES).issubset(row)
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
