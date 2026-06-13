# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for :mod:`ai.train.eval`."""

from __future__ import annotations

import json
from pathlib import Path

import numpy as np
import pytest

from ai.train import eval as eval_mod


def test_correlation_metrics_perfect() -> None:
    rng = np.random.default_rng(0)
    target = rng.uniform(0, 100, 64)
    plcc, srocc, krocc, rmse = eval_mod.correlation_metrics(target, target)
    assert plcc == pytest.approx(1.0)
    assert srocc == pytest.approx(1.0)
    assert krocc == pytest.approx(1.0)
    assert rmse == pytest.approx(0.0, abs=1e-6)


def test_correlation_metrics_anti_correlated() -> None:
    target = np.linspace(0, 1, 32)
    pred = -target
    plcc, srocc, krocc, _ = eval_mod.correlation_metrics(pred, target)
    assert plcc == pytest.approx(-1.0)
    assert srocc == pytest.approx(-1.0)
    assert krocc == pytest.approx(-1.0)


def test_correlation_metrics_shape_mismatch() -> None:
    with pytest.raises(ValueError):
        eval_mod.correlation_metrics(np.zeros(3), np.zeros(4))


def test_correlation_metrics_empty_returns_nan() -> None:
    plcc, srocc, krocc, rmse = eval_mod.correlation_metrics(np.zeros(0), np.zeros(0))
    assert np.isnan(plcc)
    assert np.isnan(srocc)
    assert np.isnan(krocc)
    assert np.isnan(rmse)


def test_evaluate_with_explicit_predictions_writes_report(tmp_path: Path) -> None:
    rng = np.random.default_rng(0)
    features = rng.standard_normal((16, 6)).astype(np.float32)
    targets = rng.uniform(0, 100, size=16).astype(np.float32)
    predictions = targets + rng.normal(0, 1.0, size=16).astype(np.float32)

    out = tmp_path / "report.json"
    report = eval_mod.evaluate(
        features=features,
        targets=targets,
        predictions=predictions,
        model_label="unit-test",
        out_path=out,
    )

    assert out.is_file()
    payload = json.loads(out.read_text())
    assert payload["n_samples"] == 16
    assert payload["model"] == "unit-test"
    assert payload["feature_dim"] == 6
    assert -1.0 <= payload["plcc"] <= 1.0
    assert -1.0 <= payload["srocc"] <= 1.0
    assert payload["rmse"] >= 0
    assert payload["latency_ms_p50_per_clip"] is None  # no ONNX path
    assert report.n_samples == 16


def test_evaluate_requires_either_onnx_or_predictions() -> None:
    with pytest.raises(ValueError):
        eval_mod.evaluate(
            features=np.zeros((4, 6), dtype=np.float32),
            targets=np.zeros(4, dtype=np.float32),
        )
    with pytest.raises(ValueError):
        eval_mod.evaluate(
            features=np.zeros((4, 6), dtype=np.float32),
            targets=np.zeros(4, dtype=np.float32),
            onnx_path=Path("model.onnx"),
            predictions=np.zeros(4, dtype=np.float32),
        )


def test_latencies_from_samples() -> None:
    p50, p95 = eval_mod.latencies_from_samples([1.0, 2.0, 3.0, 4.0, 5.0])
    assert p50 == pytest.approx(3.0)
    assert p95 == pytest.approx(4.8)
