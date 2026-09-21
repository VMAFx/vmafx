# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Contracts for the warning-free dynamo ONNX export bridge."""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnx
import onnxruntime as ort
import pytest
import torch

from aiutils.onnx_export import export_onnx


class _Add(torch.nn.Module):
    def forward(self, left: torch.Tensor, right: torch.Tensor) -> torch.Tensor:
        return left + right


class _SpatialMean(torch.nn.Module):
    """Reduction whose opset-18 form the ONNX version converter mis-converts."""

    def __init__(self) -> None:
        super().__init__()
        self.conv = torch.nn.Conv2d(1, 4, 3, padding=1)

    def forward(self, frame: torch.Tensor) -> torch.Tensor:
        pooled: torch.Tensor = self.conv(frame).mean(dim=(2, 3))
        return pooled


def test_export_converts_to_opset_17_and_preserves_shared_batch(tmp_path: Path) -> None:
    destination = tmp_path / "add.onnx"
    example = torch.zeros(2, 3, dtype=torch.float32)

    export_onnx(
        _Add().eval(),
        (example, example),
        destination,
        input_names=["left", "right"],
        output_names=["sum"],
        dynamic_axes={
            "left": {0: "batch"},
            "right": {0: "batch"},
            "sum": {0: "batch"},
        },
        opset=17,
    )

    graph = onnx.load(destination, load_external_data=False)
    assert [(item.domain, item.version) for item in graph.opset_import] == [("", 17)]
    assert [item.name for item in graph.graph.input] == ["left", "right"]
    assert [item.name for item in graph.graph.output] == ["sum"]
    assert graph.graph.input[0].type.tensor_type.shape.dim[0].dim_param == "batch"
    assert graph.graph.input[1].type.tensor_type.shape.dim[0].dim_param == "batch"
    assert graph.graph.output[0].type.tensor_type.shape.dim[0].dim_param == "batch"

    inputs = np.ones((5, 3), dtype=np.float32)
    session = ort.InferenceSession(destination, providers=["CPUExecutionProvider"])
    (actual,) = session.run(None, {"left": inputs, "right": inputs})
    np.testing.assert_array_equal(actual, inputs * 2)


def test_export_failure_preserves_existing_destination(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    destination = tmp_path / "existing.onnx"
    destination.write_bytes(b"existing")

    def fail_export(*_args: object, **_kwargs: object) -> None:
        raise RuntimeError("export failed")

    monkeypatch.setattr(torch.onnx, "export", fail_export)
    with pytest.raises(RuntimeError, match="export failed"):
        export_onnx(
            _Add().eval(),
            (torch.zeros(1, 3), torch.zeros(1, 3)),
            destination,
            input_names=["left", "right"],
            output_names=["sum"],
        )

    assert destination.read_bytes() == b"existing"
    assert list(tmp_path.iterdir()) == [destination]


def test_export_publishes_a_valid_graph_for_down_converted_reductions(tmp_path: Path) -> None:
    """ReduceMean-18 carries an attribute opset 17 does not define.

    ``onnx.version_converter`` moves ``axes`` back to an attribute but keeps
    ``noop_with_empty_axes``, so an unrepaired graph claims opset 17 while
    failing ONNX's own checker and ONNX Runtime's loader.
    """
    destination = tmp_path / "pooled.onnx"
    model = _SpatialMean().eval()

    export_onnx(
        model,
        (torch.zeros(1, 1, 16, 16, dtype=torch.float32),),
        destination,
        input_names=["frame"],
        output_names=["pooled"],
        dynamic_axes={"frame": {0: "batch"}, "pooled": {0: "batch"}},
        opset=17,
    )

    graph = onnx.load(destination, load_external_data=False)
    assert [(item.domain, item.version) for item in graph.opset_import] == [("", 17)]
    reductions = [node for node in graph.graph.node if node.op_type == "ReduceMean"]
    assert reductions, "expected the pooled mean to survive as a ReduceMean node"
    for node in reductions:
        assert "noop_with_empty_axes" not in {item.name for item in node.attribute}
    onnx.checker.check_model(graph)

    sample = np.random.default_rng(0).standard_normal((3, 1, 16, 16), dtype=np.float32)
    with torch.no_grad():
        expected = model(torch.from_numpy(sample)).numpy()
    session = ort.InferenceSession(destination, providers=["CPUExecutionProvider"])
    (actual,) = session.run(None, {"frame": sample})
    np.testing.assert_allclose(actual, expected, atol=1e-5)
