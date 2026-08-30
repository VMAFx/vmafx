# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage tests for :mod:`vmaf_train.data.feature_dump`.

The module shells out to the ``vmaf`` CLI; we mock :func:`subprocess.run` so
these tests run hermetically without needing a built binary. Round 2 of the
ai/src coverage push.
"""

from __future__ import annotations

import json
from pathlib import Path

import pandas as pd
import pytest

from vmaf_train.data import feature_dump
from vmaf_train.data.feature_dump import (
    DEFAULT_FEATURES,
    DEFAULT_VMAF_BINARY,
    Entry,
    _extractors_for,
    _lookup_feature,
    _normalize_pixfmt,
    dump_features,
)


def test_normalize_pixfmt_translates_ffmpeg_names() -> None:
    assert _normalize_pixfmt("yuv420p") == "420"
    assert _normalize_pixfmt("yuv422p") == "422"
    assert _normalize_pixfmt("yuv444p") == "444"
    assert _normalize_pixfmt("yuvj420p") == "420"
    assert _normalize_pixfmt("yuvj422p") == "422"
    assert _normalize_pixfmt("yuvj444p") == "444"


def test_normalize_pixfmt_passes_through_native_names() -> None:
    assert _normalize_pixfmt("420") == "420"
    assert _normalize_pixfmt("422") == "422"
    assert _normalize_pixfmt("444") == "444"
    # Unknown → passthrough as-is.
    assert _normalize_pixfmt("420p10le") == "420p10le"


def test_extractors_for_dedups_multiple_vif_scales() -> None:
    """Branch: same extractor referenced by multiple metric names → emit once."""
    out = _extractors_for(("vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3"))
    assert out == ["vif"]


def test_extractors_for_preserves_order_across_extractors() -> None:
    out = _extractors_for(("adm2", "vif_scale0", "motion2"))
    assert out == ["adm", "vif", "motion"]


def test_extractors_for_passes_through_unknown_metric() -> None:
    """Branch: ``_METRIC_TO_EXTRACTOR.get(m, m)`` fallback path."""
    out = _extractors_for(("ssim", "psnr"))
    assert out == ["ssim", "psnr"]


def test_extractors_for_default_features_yields_three_extractors() -> None:
    assert _extractors_for(DEFAULT_FEATURES) == ["adm", "vif", "motion"]


def test_extractors_for_empty_input_returns_empty() -> None:
    assert _extractors_for(()) == []


def test_lookup_feature_prefers_unprefixed_name() -> None:
    """Both keys present → unprefixed wins."""
    metrics = {"adm2": 0.95, "integer_adm2": 0.0}
    assert _lookup_feature(metrics, "adm2") == 0.95


def test_lookup_feature_falls_back_to_integer_prefix() -> None:
    metrics = {"integer_adm2": 0.87}
    assert _lookup_feature(metrics, "adm2") == 0.87


def test_lookup_feature_returns_none_when_neither_present() -> None:
    assert _lookup_feature({"other": 1.0}, "adm2") is None


def test_default_vmaf_binary_points_into_core_build() -> None:
    assert Path("core") / "build-cpu" / "tools" / "vmaf" == DEFAULT_VMAF_BINARY


def test_entry_defaults() -> None:
    e = Entry(key="k", ref=Path("r"), dis=Path("d"), width=16, height=16)
    assert e.pix_fmt == "420"
    assert e.bitdepth == 8
    assert e.mos is None


def test_entry_is_frozen() -> None:
    import dataclasses

    e = Entry(key="k", ref=Path("r"), dis=Path("d"), width=16, height=16)
    with pytest.raises(dataclasses.FrozenInstanceError):
        e.key = "other"  # type: ignore[misc]


def _fake_subprocess_run_writing_json(features: list[str], frames: int = 2):
    """Build a fake subprocess.run that writes a JSON doc to the ``-o`` path."""

    def _run(cmd, **_kwargs):
        # Locate the -o argument and write a synthetic JSON doc there.
        out_idx = cmd.index("-o") + 1
        out_path = Path(cmd[out_idx])
        doc = {
            "frames": [
                {
                    "frameNum": i,
                    "metrics": {f: float(i + j) for j, f in enumerate(features)},
                }
                for i in range(frames)
            ]
        }
        out_path.write_text(json.dumps(doc))

        class _Result:
            returncode = 0
            stdout = ""
            stderr = ""

        return _Result()

    return _run


def test_dump_features_writes_parquet_with_expected_columns(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    monkeypatch.setattr(
        feature_dump.subprocess, "run", _fake_subprocess_run_writing_json(list(DEFAULT_FEATURES), 3)
    )
    entries = [
        Entry(
            key="clip-a",
            ref=tmp_path / "ref.yuv",
            dis=tmp_path / "dis.yuv",
            width=320,
            height=240,
            mos=55.5,
        )
    ]
    out_parquet = tmp_path / "nested" / "out.parquet"

    result = dump_features(entries, out_parquet)
    assert result == out_parquet
    assert out_parquet.exists()
    assert out_parquet.parent.exists()  # parents created

    df = pd.read_parquet(out_parquet)
    assert set(["key", "frame", "mos", *DEFAULT_FEATURES]) <= set(df.columns)
    assert len(df) == 3
    assert (df["key"] == "clip-a").all()
    assert (df["mos"] == 55.5).all()


def test_dump_features_with_explicit_features_subset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """Custom feature tuple is honoured both in CLI args and in the output schema."""
    captured: dict[str, list[str]] = {}

    def _run(cmd, **_kwargs):
        captured["cmd"] = list(cmd)
        out_idx = cmd.index("-o") + 1
        out_path = Path(cmd[out_idx])
        out_path.write_text(json.dumps({"frames": [{"frameNum": 0, "metrics": {"adm2": 0.9}}]}))

        class _R:
            returncode = 0

        return _R()

    monkeypatch.setattr(feature_dump.subprocess, "run", _run)
    entries = [Entry(key="k", ref=tmp_path / "r", dis=tmp_path / "d", width=16, height=16)]
    out_parquet = tmp_path / "out.parquet"
    dump_features(entries, out_parquet, features=("adm2",))
    # --feature adm appears (after dedupe) and only once.
    cmd = captured["cmd"]
    assert cmd.count("--feature") == 1
    assert cmd[cmd.index("--feature") + 1] == "adm"


def test_dump_features_uses_integer_fallback_lookup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """When the metric appears only as ``integer_<name>`` we still record it."""

    def _run(cmd, **_kwargs):
        out_idx = cmd.index("-o") + 1
        Path(cmd[out_idx]).write_text(
            json.dumps({"frames": [{"frameNum": 0, "metrics": {"integer_adm2": 0.42}}]})
        )

        class _R:
            returncode = 0

        return _R()

    monkeypatch.setattr(feature_dump.subprocess, "run", _run)
    entries = [Entry(key="k", ref=tmp_path / "r", dis=tmp_path / "d", width=16, height=16)]
    out_parquet = tmp_path / "out.parquet"
    dump_features(entries, out_parquet, features=("adm2",))
    df = pd.read_parquet(out_parquet)
    assert df.loc[0, "adm2"] == pytest.approx(0.42)


def test_dump_features_pixfmt_is_normalised_in_cli(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    captured: dict[str, list[str]] = {}

    def _run(cmd, **_kwargs):
        captured["cmd"] = list(cmd)
        out_idx = cmd.index("-o") + 1
        Path(cmd[out_idx]).write_text(json.dumps({"frames": []}))

        class _R:
            returncode = 0

        return _R()

    monkeypatch.setattr(feature_dump.subprocess, "run", _run)
    entries = [
        Entry(
            key="k",
            ref=tmp_path / "r",
            dis=tmp_path / "d",
            width=16,
            height=16,
            pix_fmt="yuv422p",
        )
    ]
    dump_features(entries, tmp_path / "out.parquet", features=("adm2",))
    cmd = captured["cmd"]
    assert cmd[cmd.index("-p") + 1] == "422"
