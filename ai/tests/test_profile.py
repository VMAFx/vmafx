# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke test for the model profiler — confirms timing + shape inference."""

from __future__ import annotations

from pathlib import Path

import onnx
from onnx import TensorProto, helper

from vmaf_train.profile import profile_model, render_table


def _tiny_mlp(path: Path, in_features: int = 6) -> None:
    # Dynamic batch dim so custom-shape profiling works.
    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, ["N", in_features])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, ["N", 1])
    w = helper.make_tensor("W", TensorProto.FLOAT, [in_features, 1], [0.1] * in_features)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [0.0])
    node = helper.make_node("Gemm", ["x", "W", "B"], ["y"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def test_profile_infers_shape_from_graph(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = profile_model(p, warmup=1, iters=3)
    assert report.results
    r = report.results[0]
    assert r.shape == (1, 6)
    assert r.mean_ms >= 0.0
    assert r.iters == 3


def test_profile_custom_shape(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p, in_features=6)
    report = profile_model(p, shapes=[(8, 6)], warmup=1, iters=5)
    assert report.results[0].shape == (8, 6)


def test_unknown_provider_raises(tmp_path: Path) -> None:
    import pytest

    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    with pytest.raises(ValueError):
        profile_model(p, providers=["DoesNotExistExecutionProvider"], iters=1)


def test_report_serializes(tmp_path: Path) -> None:
    p = tmp_path / "m.onnx"
    _tiny_mlp(p)
    report = profile_model(p, warmup=1, iters=2)
    d = report.to_dict()
    assert d["model"] == str(p)
    assert isinstance(d["results"][0]["shape"], list)
    table = render_table(report)
    assert "mean" in table and "provider" in table
