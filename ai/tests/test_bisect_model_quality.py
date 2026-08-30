# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Bisect-model-quality: binary-search a timeline of ONNX models for the
first one that trips a PLCC / SROCC / RMSE gate.

We build a synthetic timeline of FR ONNX models where model_k(x) ≈ x · w_k
with w_k progressively detuned so correlation with the target collapses
somewhere in the middle of the list. The bisect must localise that
transition in log₂(N) steps.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnx
import pytest
from onnx import TensorProto, helper

from vmaf_train.bisect_model_quality import bisect_model_quality, render_table

N_FEATURES = 6


def _save_linear_fr(path: Path, weights: np.ndarray, bias: float = 0.0) -> None:
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", N_FEATURES])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N"])
    w = helper.make_tensor(
        "W", TensorProto.FLOAT, [N_FEATURES, 1], weights.astype(np.float32).flatten().tolist()
    )
    b = helper.make_tensor("b", TensorProto.FLOAT, [1], [float(bias)])
    axes = helper.make_tensor("axes", TensorProto.INT64, [1], [1])
    mm = helper.make_node("MatMul", ["input", "W"], ["mm"])
    add = helper.make_node("Add", ["mm", "b"], ["wide"])
    sq = helper.make_node("Squeeze", ["wide", "axes"], ["score"])
    graph = helper.make_graph([mm, add, sq], "fr_linear", [x], [y], [w, b, axes])
    onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]), str(path))


def _make_timeline(tmp_path: Path, n: int, bad_from: int) -> list[Path]:
    """Build @p n checkpoints; index ``bad_from`` and later score random
    output (detuned weights) so their PLCC collapses."""
    rng = np.random.default_rng(0)
    good_w = np.ones(N_FEATURES) / N_FEATURES
    paths: list[Path] = []
    for i in range(n):
        if i < bad_from:
            # Tiny perturbation around the good weights so PLCC is still ~1.
            w = good_w + rng.normal(0, 1e-4, size=N_FEATURES)
        else:
            # Randomised weights → essentially uncorrelated with the target.
            w = rng.normal(0, 1.0, size=N_FEATURES)
        p = tmp_path / f"model_{i:02d}.onnx"
        _save_linear_fr(p, w)
        paths.append(p)
    return paths


def _data() -> tuple[np.ndarray, np.ndarray]:
    rng = np.random.default_rng(1)
    feats = rng.uniform(0.2, 0.9, size=(32, N_FEATURES)).astype(np.float32)
    # Targets proportional to the sum of features — good_w model predicts this well.
    targets = feats.sum(axis=1) / N_FEATURES
    return feats, targets


def test_bisect_localises_first_bad(tmp_path: Path) -> None:
    pytest.importorskip("onnxruntime")
    paths = _make_timeline(tmp_path, n=9, bad_from=5)
    feats, targets = _data()
    result = bisect_model_quality(paths, feats, targets, min_plcc=0.9, input_name="input")
    assert result.first_bad_index == 5
    assert result.last_good_index == 4
    # log2(9) ≈ 3.17; plus the 2 boundary probes → ≤ 6 total visits.
    assert len(result.steps) <= 6


def test_no_regression_verdict(tmp_path: Path) -> None:
    pytest.importorskip("onnxruntime")
    # Everything-good timeline: bad_from == n, i.e. no model is bad.
    paths = _make_timeline(tmp_path, n=4, bad_from=4)
    feats, targets = _data()
    result = bisect_model_quality(paths, feats, targets, min_plcc=0.9)
    assert result.first_bad_index is None
    assert "no regression" in result.verdict


def test_first_model_already_bad(tmp_path: Path) -> None:
    pytest.importorskip("onnxruntime")
    paths = _make_timeline(tmp_path, n=4, bad_from=0)
    feats, targets = _data()
    result = bisect_model_quality(paths, feats, targets, min_plcc=0.9)
    assert result.first_bad_index == 0
    assert "nothing to bisect" in result.verdict


def test_rmse_gate_works(tmp_path: Path) -> None:
    pytest.importorskip("onnxruntime")
    paths = _make_timeline(tmp_path, n=5, bad_from=3)
    feats, targets = _data()
    result = bisect_model_quality(paths, feats, targets, max_rmse=0.05)
    # RMSE gate must flip at the same index as the PLCC gate on this data.
    assert result.first_bad_index == 3


def test_requires_exactly_one_threshold() -> None:
    with pytest.raises(ValueError, match="exactly one"):
        bisect_model_quality([Path("a"), Path("b")], np.zeros((2, 6)), np.zeros(2))
    with pytest.raises(ValueError, match="exactly one"):
        bisect_model_quality(
            [Path("a"), Path("b")],
            np.zeros((2, 6)),
            np.zeros(2),
            min_plcc=0.9,
            max_rmse=1.0,
        )


def test_requires_at_least_two_models() -> None:
    with pytest.raises(ValueError, match="at least 2"):
        bisect_model_quality([Path("a")], np.zeros((2, 6)), np.zeros(2), min_plcc=0.9)


def test_render_table_contains_verdict(tmp_path: Path) -> None:
    pytest.importorskip("onnxruntime")
    paths = _make_timeline(tmp_path, n=5, bad_from=2)
    feats, targets = _data()
    result = bisect_model_quality(paths, feats, targets, min_plcc=0.9)
    table = render_table(result)
    assert "PLCC" in table and "SROCC" in table
    assert "first bad" in table
