# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``vmaf_train.validate_norm``.

The existing ``test_validate_norm.py`` covers the matched-distribution +
mean-drift happy paths. This file closes the remaining branches: missing
sidecar (FileNotFoundError), parquet without any FEATURE_COLUMNS,
``render_table`` for an empty-drifts report, and the warnings tail in
``render_table``.
"""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from vmaf_train.features import FEATURE_COLUMNS
from vmaf_train.validate_norm import NormReport, render_table, validate_norm


def _write_features(tmp: Path, mean: list[float], std: list[float], n: int = 256) -> Path:
    rng = np.random.default_rng(0)
    cols = {
        c: rng.normal(mean[i], std[i], size=n).astype(np.float32)
        for i, c in enumerate(FEATURE_COLUMNS)
    }
    path = tmp / "f.parquet"
    pd.DataFrame(cols).to_parquet(path)
    return path


def test_missing_sidecar_raises_file_not_found(tmp_path: Path) -> None:
    """``validate_norm`` raises FileNotFoundError when the sidecar is absent."""
    onnx_path = tmp_path / "missing.onnx"
    onnx_path.write_bytes(b"")
    # No sidecar JSON is written.
    features = _write_features(tmp_path, [0.0] * len(FEATURE_COLUMNS), [1.0] * len(FEATURE_COLUMNS))

    with pytest.raises(FileNotFoundError, match="sidecar not found"):
        validate_norm(onnx_path, features)


def test_features_without_any_expected_columns_raises_value_error(tmp_path: Path) -> None:
    """A parquet that has none of the FEATURE_COLUMNS raises ValueError."""
    onnx_path = tmp_path / "m.onnx"
    onnx_path.write_bytes(b"")
    sidecar = onnx_path.with_suffix(".json")
    sidecar.write_text(
        json.dumps(
            {
                "schema_version": 1,
                "name": "m",
                "kind": "fr",
                "normalization": {
                    "mean": [0.0] * len(FEATURE_COLUMNS),
                    "std": [1.0] * len(FEATURE_COLUMNS),
                },
            }
        )
    )

    features = tmp_path / "bogus.parquet"
    pd.DataFrame({"unrelated": [1, 2, 3]}).to_parquet(features)

    with pytest.raises(ValueError, match="none of the expected feature columns"):
        validate_norm(onnx_path, features)


def test_render_table_for_no_drifts_report(tmp_path: Path) -> None:
    """``render_table`` for a NormReport with no drifts emits the short form."""
    sidecar = tmp_path / "no_norm.onnx.json"
    report = NormReport(sidecar=sidecar, n_samples=0)
    out = render_table(report)
    assert "no normalization block" in out


def test_render_table_for_no_drifts_with_warnings(tmp_path: Path) -> None:
    """``render_table`` for a no-drifts NormReport surfaces the first warning."""
    sidecar = tmp_path / "warn.onnx.json"
    report = NormReport(sidecar=sidecar, n_samples=0)
    report.warnings.append("custom upstream warning")
    out = render_table(report)
    assert "custom upstream warning" in out


def test_render_table_appends_warnings_tail(tmp_path: Path) -> None:
    """Lines 157-159: render_table prints a ``WARN:`` line for each warning."""
    mean = [0.5] * len(FEATURE_COLUMNS)
    std = [0.1] * len(FEATURE_COLUMNS)
    real_mean = [0.95] * len(FEATURE_COLUMNS)

    onnx_path = tmp_path / "drift.onnx"
    onnx_path.write_bytes(b"")
    onnx_path.with_suffix(".json").write_text(
        json.dumps(
            {
                "schema_version": 1,
                "name": "drift",
                "kind": "fr",
                "normalization": {"mean": mean, "std": std},
            }
        )
    )
    features = _write_features(tmp_path, real_mean, std)

    report = validate_norm(onnx_path, features)
    assert report.warnings  # sanity: drift produced at least one warning
    rendered = render_table(report)
    assert "WARN:" in rendered
