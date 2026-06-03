# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Branch-coverage top-up for ``vmaf_train.cross_backend``.

Targets the remaining branches not exercised by ``test_cross_backend.py``:
the no-graph-input shape inference guard (line 80), and the
``render_table`` branches for an empty-comparisons report (line 145) and
the ``missing`` providers tail (lines 152-153).
"""

from __future__ import annotations

from pathlib import Path

import onnx
import pytest
from onnx import TensorProto, helper

from vmaf_train.cross_backend import (
    CPU_PROVIDER,
    BackendComparison,
    CrossBackendReport,
    compare_backends,
    render_table,
)


def _empty_input_model(path: Path) -> None:
    """Build a degenerate ONNX model whose graph has zero inputs.

    Used to exercise the ``raise ValueError`` guard in
    ``_infer_or_given_shape``.
    """
    out = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1])
    const = helper.make_node("Constant", inputs=[], outputs=["y"], value_float=1.0)
    graph = helper.make_graph([const], "no_in", [], [out])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def test_no_graph_input_raises_value_error(tmp_path: Path) -> None:
    """A model with no graph inputs raises ValueError before session creation."""
    p = tmp_path / "no_inputs.onnx"
    _empty_input_model(p)

    with pytest.raises(ValueError, match="no inputs"):
        compare_backends(p, providers=[CPU_PROVIDER])


def test_render_table_for_empty_comparisons() -> None:
    """``render_table`` for a report with no comparisons emits the CPU-only hint (line 145)."""
    report = CrossBackendReport(model=Path("m.onnx"), atol=1e-3)
    rendered = render_table(report)
    assert "no alternate providers available" in rendered


def test_render_table_includes_missing_providers_tail() -> None:
    """``render_table`` appends a ``requested but unavailable`` line when missing[] non-empty."""
    report = CrossBackendReport(model=Path("m.onnx"), atol=1e-3)
    report.comparisons.append(
        BackendComparison(
            provider=CPU_PROVIDER,
            max_abs_error=0.0,
            mean_abs_error=0.0,
            shape=(1, 6),
        )
    )
    report.missing = ["CUDAExecutionProvider", "OpenVINOExecutionProvider"]

    rendered = render_table(report)

    assert "requested but unavailable" in rendered
    assert "CUDAExecutionProvider" in rendered
    assert "OpenVINOExecutionProvider" in rendered
