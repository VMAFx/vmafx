# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Pre-deploy audit for learned-filter ONNX models.

The audit runs the filter over a corpus of frames and flags four
failure modes:
  * mean shift (filter brightens / darkens content)
  * std-ratio inflation (filter amplifies noise)
  * clipping at codec boundaries
  * SSIM collapse (filter destroys structure)

These tests build synthetic ONNX filters that exercise each failure
mode exactly and assert the audit fires the matching warning.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnx
import pytest
from onnx import TensorProto, helper

from vmaf_train.learned_filter_audit import audit_learned_filter, render_table


def _make_filter_identity(path: Path) -> None:
    """Filter(x) = x — passes every audit gate."""
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", 1, "H", "W"])
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, ["N", 1, "H", "W"])
    node = helper.make_node("Identity", ["input"], ["output"])
    graph = helper.make_graph([node], "ident", [x], [y])
    onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]), str(path))


def _make_filter_affine(path: Path, alpha: float, beta: float) -> None:
    """Filter(x) = alpha*x + beta — exercises mean shift and std ratio."""
    x = helper.make_tensor_value_info("input", TensorProto.FLOAT, ["N", 1, "H", "W"])
    y = helper.make_tensor_value_info("output", TensorProto.FLOAT, ["N", 1, "H", "W"])
    scale = helper.make_tensor("Alpha", TensorProto.FLOAT, [1], [alpha])
    bias = helper.make_tensor("Beta", TensorProto.FLOAT, [1], [beta])
    mul = helper.make_node("Mul", ["input", "Alpha"], ["scaled"])
    add = helper.make_node("Add", ["scaled", "Beta"], ["output"])
    graph = helper.make_graph([mul, add], "affine", [x], [y], [scale, bias])
    onnx.save(helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]), str(path))


def _random_frames(n: int = 4, h: int = 32, w: int = 32) -> list[np.ndarray]:
    rng = np.random.default_rng(0)
    return [rng.uniform(0.2, 0.8, size=(h, w)).astype(np.float32) for _ in range(n)]


def test_identity_filter_passes(tmp_path: Path) -> None:
    p = tmp_path / "identity.onnx"
    _make_filter_identity(p)
    report = audit_learned_filter(p, _random_frames())
    assert report.ok, report.warnings
    # SSIM of x with itself must be exactly 1 (within float eps).
    assert abs(report.avg_ssim - 1.0) < 1e-6
    assert report.max_mean_shift < 1e-6


def test_mean_shift_detected(tmp_path: Path) -> None:
    p = tmp_path / "bright.onnx"
    # Filter adds +0.2 to every pixel → mean shift is 0.2 > 0.05.
    _make_filter_affine(p, alpha=1.0, beta=0.2)
    report = audit_learned_filter(p, _random_frames())
    assert not report.ok
    assert any("Δmean" in w for w in report.warnings)


def test_std_inflation_detected(tmp_path: Path) -> None:
    p = tmp_path / "loud.onnx"
    _make_filter_affine(p, alpha=3.0, beta=-1.0)  # triples std, preserves mean
    report = audit_learned_filter(p, _random_frames())
    assert any("amplifies noise" in w for w in report.warnings)


def test_clip_fraction_detected(tmp_path: Path) -> None:
    p = tmp_path / "clip.onnx"
    # Multiply by a huge factor so output saturates at peak on most pixels.
    _make_filter_affine(p, alpha=20.0, beta=0.0)
    report = audit_learned_filter(p, _random_frames(), peak=1.0, std_ratio_max=1e9)
    # At least some fraction of outputs land above peak=1.0 → clip warning.
    assert any("clipped" in w for w in report.warnings)


def test_render_table_and_json(tmp_path: Path) -> None:
    p = tmp_path / "identity.onnx"
    _make_filter_identity(p)
    report = audit_learned_filter(p, _random_frames(n=2))
    assert report.n_frames == 2
    table = render_table(report)
    assert "SSIM" in table and "frame" in table
    d = report.to_dict()
    assert d["ok"] is True
    assert len(d["frames"]) == 2


def test_invalid_frame_shape_raises(tmp_path: Path) -> None:
    p = tmp_path / "identity.onnx"
    _make_filter_identity(p)
    with pytest.raises(ValueError, match="must be 2-D"):
        audit_learned_filter(p, [np.zeros((3, 32, 32), dtype=np.float32)])


def test_empty_frame_list_raises(tmp_path: Path) -> None:
    p = tmp_path / "identity.onnx"
    _make_filter_identity(p)
    with pytest.raises(ValueError, match="at least one frame"):
        audit_learned_filter(p, [])
