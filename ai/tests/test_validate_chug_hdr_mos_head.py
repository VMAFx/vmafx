# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent OR MIT
"""Tests for the CHUG HDR MOS head held-out test validator.

Covers:
* schema column derivation (``_row_to_features`` with the chug-hdr-wide-v1
  34-column schema — imported from the shared trainer)
* split filtering — only ``split == "test"`` rows are used
* PLCC / SROCC / RMSE metric computation
* gate logic (pass when all three thresholds are met; fail when any misses)
* end-to-end smoke run via ``main()`` against a small synthetic JSONL fixture
  (no real corpus or ONNX on disk)
"""

from __future__ import annotations

import importlib.util
import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

np = pytest.importorskip("numpy")

import train_konvid_mos_head as trainer  # noqa: E402

# Load validator under test without executing __main__.
_SCRIPT_PATH = REPO_ROOT / "ai" / "scripts" / "validate_chug_hdr_mos_head.py"
_SPEC = importlib.util.spec_from_file_location("validate_chug_hdr_mos_head", _SCRIPT_PATH)
assert _SPEC is not None and _SPEC.loader is not None
validator = importlib.util.module_from_spec(_SPEC)
sys.modules[_SPEC.name] = validator
_SPEC.loader.exec_module(validator)


# ---------------------------------------------------------------------------
# Helpers — synthetic JSONL fixture writer.
# ---------------------------------------------------------------------------


def _write_chug_jsonl(path: Path, n_train: int, n_val: int, n_test: int) -> None:
    """Write a minimal CHUG feature JSONL with the three splits."""
    rng = np.random.default_rng(42)
    rows: list[dict] = []
    for i in range(n_train + n_val + n_test):
        if i < n_train:
            split = "train"
        elif i < n_train + n_val:
            split = "val"
        else:
            split = "test"
        rows.append(
            {
                "split": split,
                "mos": float(rng.uniform(trainer.MOS_MIN, trainer.MOS_MAX)),
                "adm2": float(rng.uniform(0.5, 1.0)),
                "vif_scale0": float(rng.uniform(0.2, 0.9)),
                "vif_scale1": float(rng.uniform(0.3, 0.9)),
                "vif_scale2": float(rng.uniform(0.4, 0.9)),
                "vif_scale3": float(rng.uniform(0.5, 0.9)),
                "motion2": float(rng.uniform(0.0, 15.0)),
                # temporal aggregates for the wide schema
                "adm2_p10": float(rng.uniform(0.4, 0.8)),
                "adm2_p90": float(rng.uniform(0.8, 1.0)),
                "adm2_std": float(rng.uniform(0.01, 0.1)),
                "chug_bitrate_label": "1M",
                "chug_orientation": "Landscape",
                "chug_ref": 0,
                "width": 1920,
                "height": 1080,
                "feature_width": 1920,
                "feature_height": 1080,
                "duration_s": 8.0,
                "n_feature_frames": 240,
                "feature_bitdepth": 10,
            }
        )
    path.write_text(
        "\n".join(json.dumps(row) for row in rows) + "\n",
        encoding="utf-8",
    )


# ---------------------------------------------------------------------------
# Schema column derivation.
# ---------------------------------------------------------------------------


def test_chug_hdr_wide_schema_has_34_columns() -> None:
    assert len(trainer.CHUG_HDR_FEATURE_COLUMNS) == 34


def test_row_to_features_derives_wide_schema_from_synthetic_row() -> None:
    """_row_to_features must project the 34 chug-hdr-wide-v1 columns."""
    row = {
        "mos": 3.0,
        "adm2": 0.80,
        "vif_scale0": 0.60,
        "vif_scale1": 0.65,
        "vif_scale2": 0.70,
        "vif_scale3": 0.75,
        "motion2": 5.0,
        "adm2_p10": 0.72,
        "adm2_p90": 0.88,
        "adm2_std": 0.04,
        "chug_bitrate_label": "2M",
        "chug_orientation": "Landscape",
        "chug_ref": 0,
        "width": 3840,
        "height": 2160,
        "feature_width": 3840,
        "feature_height": 2160,
        "duration_s": 10.0,
        "n_feature_frames": 300,
        "feature_bitdepth": 10,
    }
    result = trainer._row_to_features(row, feature_columns=trainer.CHUG_HDR_FEATURE_COLUMNS)
    assert result is not None
    feats, mos = result
    assert feats.shape == (34,)
    assert mos == pytest.approx(3.0)
    idx = trainer.CHUG_HDR_FEATURE_COLUMNS.index
    assert feats[idx("chug_bitrate_mbps")] == pytest.approx(2.0)
    assert feats[idx("chug_orientation_portrait")] == pytest.approx(0.0)
    assert feats[idx("distorted_width_norm")] == pytest.approx(1.0)
    assert feats[idx("feature_bitdepth_norm")] == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# Split filtering.
# ---------------------------------------------------------------------------


def test_load_test_rows_filters_to_test_split_only(tmp_path: Path) -> None:
    p = tmp_path / "shard_00.features.jsonl"
    _write_chug_jsonl(p, n_train=5, n_val=3, n_test=4)

    feature_list, mos_list = validator._load_test_rows([p])

    assert len(feature_list) == 4
    assert len(mos_list) == 4
    # All MOS values are in the valid range.
    for mos in mos_list:
        assert trainer.MOS_MIN <= mos <= trainer.MOS_MAX


def test_load_test_rows_skips_train_and_val(tmp_path: Path) -> None:
    p = tmp_path / "shard_00.features.jsonl"
    rows = [
        {"split": "train", "mos": 3.0, "adm2": 0.8},
        {"split": "val", "mos": 3.1, "adm2": 0.7},
        {"split": "test", "mos": 3.2, "adm2": 0.9},
        {"split": "test", "mos": 2.8, "adm2": 0.6},
    ]
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")

    feature_list, mos_list = validator._load_test_rows([p])

    assert len(feature_list) == 2
    assert mos_list == pytest.approx([3.2, 2.8])


def test_load_test_rows_drops_row_without_mos(tmp_path: Path) -> None:
    p = tmp_path / "shard_00.features.jsonl"
    rows = [
        {"split": "test", "mos": 3.0},
        {"split": "test"},  # missing MOS — must be dropped
        {"split": "test", "mos": 2.5},
    ]
    p.write_text("\n".join(json.dumps(r) for r in rows) + "\n", encoding="utf-8")

    feature_list, _mos_list = validator._load_test_rows([p])

    assert len(feature_list) == 2


# ---------------------------------------------------------------------------
# Metric computation.
# ---------------------------------------------------------------------------


def test_plcc_perfect_correlation() -> None:
    a = np.arange(1.0, 6.0)
    b = 2.0 * a + 1.0
    assert validator._plcc(a, b) == pytest.approx(1.0)


def test_plcc_anti_correlation() -> None:
    a = np.arange(1.0, 6.0)
    b = -a
    assert validator._plcc(a, b) == pytest.approx(-1.0)


def test_srocc_perfect_rank_agreement() -> None:
    a = np.array([1.0, 2.0, 3.0, 4.0, 5.0])
    b = np.array([10.0, 20.0, 30.0, 40.0, 50.0])
    assert validator._srocc(a, b) == pytest.approx(1.0)


def test_rmse_zero_on_identical_arrays() -> None:
    a = np.array([2.0, 3.0, 4.0])
    assert validator._rmse(a, a) == pytest.approx(0.0)


def test_rmse_known_value() -> None:
    a = np.array([1.0, 2.0, 3.0])
    b = np.array([2.0, 2.0, 2.0])
    # errors: -1, 0, 1 → MSE = (1+0+1)/3 → RMSE = sqrt(2/3)
    assert validator._rmse(a, b) == pytest.approx(np.sqrt(2.0 / 3.0))


# ---------------------------------------------------------------------------
# Gate logic.
# ---------------------------------------------------------------------------


def test_build_gate_verdict_pass_when_all_thresholds_met() -> None:
    verdict = validator._build_gate_verdict(plcc=0.90, srocc=0.88, rmse=0.30)
    assert verdict["passed"] is True


def test_build_gate_verdict_fail_on_low_plcc() -> None:
    verdict = validator._build_gate_verdict(
        plcc=0.80,  # below 0.85
        srocc=0.88,
        rmse=0.30,
    )
    assert verdict["passed"] is False


def test_build_gate_verdict_fail_on_low_srocc() -> None:
    verdict = validator._build_gate_verdict(
        plcc=0.90,
        srocc=0.75,  # below 0.82
        rmse=0.30,
    )
    assert verdict["passed"] is False


def test_build_gate_verdict_fail_on_high_rmse() -> None:
    verdict = validator._build_gate_verdict(
        plcc=0.90,
        srocc=0.88,
        rmse=0.50,  # above 0.45
    )
    assert verdict["passed"] is False


def test_build_gate_verdict_fail_on_nan_plcc() -> None:
    verdict = validator._build_gate_verdict(
        plcc=float("nan"),
        srocc=0.88,
        rmse=0.30,
    )
    assert verdict["passed"] is False


# ---------------------------------------------------------------------------
# End-to-end: main() on a synthetic JSONL (no real ONNX on disk).
# Tests the CLI argument parsing and split-filtering path without inference.
# ---------------------------------------------------------------------------


def test_main_returns_1_when_onnx_missing(tmp_path: Path) -> None:
    p = tmp_path / "shard_00.features.jsonl"
    _write_chug_jsonl(p, n_train=5, n_val=3, n_test=4)

    rc = validator.main(
        [
            "--onnx",
            str(tmp_path / "nonexistent.onnx"),
            "--feature-jsonl",
            str(p),
            "--out-json",
            str(tmp_path / "report.json"),
            "--out-md",
            str(tmp_path / "report.md"),
        ]
    )

    assert rc == 1


def test_main_returns_1_when_no_shards_found(tmp_path: Path) -> None:
    """Empty shard dir with no feature-jsonl argument returns exit 1."""
    rc = validator.main(
        [
            "--onnx",
            str(tmp_path / "model.onnx"),
            "--shard-dir",
            str(tmp_path),
            "--out-json",
            str(tmp_path / "report.json"),
            "--out-md",
            str(tmp_path / "report.md"),
        ]
    )

    assert rc == 1


def test_discover_shards_returns_sorted_paths(tmp_path: Path) -> None:
    for name in ("shard_02.features.jsonl", "shard_00.features.jsonl", "shard_01.features.jsonl"):
        (tmp_path / name).write_text('{"split":"test","mos":3.0}\n', encoding="utf-8")
    (tmp_path / "unrelated.jsonl").write_text("", encoding="utf-8")

    found = validator._discover_shards(tmp_path)

    assert [p.name for p in found] == [
        "shard_00.features.jsonl",
        "shard_01.features.jsonl",
        "shard_02.features.jsonl",
    ]


def test_gate_constants_match_adr_0325() -> None:
    assert pytest.approx(0.85) == validator.GATE_PLCC_MIN
    assert pytest.approx(0.82) == validator.GATE_SROCC_MIN
    assert pytest.approx(0.45) == validator.GATE_RMSE_MAX
