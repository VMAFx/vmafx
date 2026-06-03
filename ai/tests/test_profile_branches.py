# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``vmaf_train.profile``.

Targets the two unexercised guards: the no-graph-input ValueError and the
unavailable-provider ValueError.
"""

from __future__ import annotations

from pathlib import Path

import onnx
import pytest
from onnx import TensorProto, helper

from vmaf_train.profile import _available_providers, _infer_input_shape, profile_model


def _model_no_inputs(path: Path) -> None:
    out = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])
    const = helper.make_node("Constant", inputs=[], outputs=["y"], value_float=0.0)
    graph = helper.make_graph([const], "no_in", [], [out])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def test_infer_input_shape_raises_when_no_graph_inputs(tmp_path: Path) -> None:
    """_infer_input_shape raises ValueError when the graph has no inputs."""
    p = tmp_path / "empty_in.onnx"
    _model_no_inputs(p)
    with pytest.raises(ValueError, match="no graph inputs"):
        _infer_input_shape(p)


def test_available_providers_raises_for_unavailable_requested() -> None:
    """Requesting an EP that ORT does not list raises ValueError."""
    with pytest.raises(ValueError, match="not available"):
        _available_providers(["DefinitelyNotAnExecutionProvider"])


def test_available_providers_returns_all_when_requested_is_none() -> None:
    """None means "use everything ORT exposes" — non-empty list."""
    eps = _available_providers(None)
    assert eps
    assert "CPUExecutionProvider" in eps


def test_profile_model_propagates_unavailable_provider_error(tmp_path: Path) -> None:
    """profile_model bubbles up the _available_providers ValueError."""
    p = tmp_path / "no_in.onnx"
    _model_no_inputs(p)
    with pytest.raises(ValueError):
        profile_model(
            p,
            shapes=[(1,)],
            providers=["DefinitelyNotAnExecutionProvider"],
            warmup=1,
            iters=1,
        )
