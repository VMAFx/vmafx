# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Feature-normalization validator flags distribution drift."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from vmaf_train.features import FEATURE_COLUMNS
from vmaf_train.validate_norm import render_table, validate_norm


def _write_sidecar(tmp: Path, mean: list[float], std: list[float]) -> Path:
    onnx_path = tmp / "m.onnx"
    onnx_path.write_bytes(b"")  # loader only reads the .json sibling
    sidecar = onnx_path.with_suffix(".json")
    sidecar.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "name": "m",
                "kind": "fr",
                "normalization": {"mean": mean, "std": std},
            }
        )
    )
    return onnx_path


def _write_features(tmp: Path, mean: list[float], std: list[float], n: int = 500) -> Path:
    rng = np.random.default_rng(0)
    cols = {
        c: rng.normal(mean[i], std[i], size=n).astype(np.float32)
        for i, c in enumerate(FEATURE_COLUMNS)
    }
    cols["mos"] = rng.uniform(20, 90, size=n).astype(np.float32)
    path = tmp / "f.parquet"
    pd.DataFrame(cols).to_parquet(path)
    return path


def test_matching_distributions_clean(tmp_path: Path) -> None:
    mean = [0.5] * len(FEATURE_COLUMNS)
    std = [0.1] * len(FEATURE_COLUMNS)
    model = _write_sidecar(tmp_path, mean, std)
    features = _write_features(tmp_path, mean, std)
    report = validate_norm(model, features)
    assert report.ok, report.warnings
    assert report.n_samples == 500


def test_mean_drift_flagged(tmp_path: Path) -> None:
    declared_mean = [0.5] * len(FEATURE_COLUMNS)
    std = [0.1] * len(FEATURE_COLUMNS)
    # Real data has a mean shift of >3σ on every feature.
    real_mean = [0.9] * len(FEATURE_COLUMNS)
    model = _write_sidecar(tmp_path, declared_mean, std)
    features = _write_features(tmp_path, real_mean, std)
    report = validate_norm(model, features)
    assert not report.ok
    # Every feature should have flagged a mean drift.
    assert any("drift" in w for w in report.warnings)


def test_missing_normalization_warning(tmp_path: Path) -> None:
    onnx_path = tmp_path / "m.onnx"
    onnx_path.write_bytes(b"")
    onnx_path.with_suffix(".json").write_text(
        json.dumps({"schema_version": 1, "name": "m", "kind": "fr", "normalization": {}})
    )
    features = _write_features(tmp_path, [0.0] * len(FEATURE_COLUMNS), [1.0] * len(FEATURE_COLUMNS))
    report = validate_norm(onnx_path, features)
    assert report.warnings
    assert "no normalization" in report.warnings[0]


def test_sidecar_column_count_mismatch(tmp_path: Path) -> None:
    model = _write_sidecar(tmp_path, mean=[0.0], std=[1.0])  # wrong length
    features = _write_features(tmp_path, [0.0] * len(FEATURE_COLUMNS), [1.0] * len(FEATURE_COLUMNS))
    with pytest.raises(ValueError):
        validate_norm(model, features)


def test_render_table(tmp_path: Path) -> None:
    mean = [0.5] * len(FEATURE_COLUMNS)
    std = [0.1] * len(FEATURE_COLUMNS)
    model = _write_sidecar(tmp_path, mean, std)
    features = _write_features(tmp_path, mean, std)
    table = render_table(validate_norm(model, features))
    assert "feature" in table and "samples" in table


def test_json_roundtrip(tmp_path: Path) -> None:
    mean = [0.5] * len(FEATURE_COLUMNS)
    std = [0.1] * len(FEATURE_COLUMNS)
    model = _write_sidecar(tmp_path, mean, std)
    features = _write_features(tmp_path, mean, std)
    report = validate_norm(model, features)
    data = report.to_dict()
    assert data["n_samples"] == 500
    assert len(data["drifts"]) == len(FEATURE_COLUMNS)
