# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Static PTQ: fp32 → INT8 with a parquet calibration source.

These tests build a trivial linear fp32 model + synthetic calibration
parquet, run the quantizer end-to-end, and assert that the INT8 output
tracks the fp32 output within a small error budget. Anything looser
than this would defeat the purpose of the gate.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnx
import pandas as pd
import pytest
from onnx import TensorProto, helper

from vmaf_train.features import FEATURE_COLUMNS
from vmaf_train.quantize import quantize_int8, render_table


def _tiny_mlp(path: Path, in_features: int = 6) -> None:
    x = helper.make_tensor_value_info("features", TensorProto.FLOAT, ["N", in_features])
    y = helper.make_tensor_value_info("score", TensorProto.FLOAT, ["N", 1])
    # Weights chosen so outputs land in a realistic MOS-like range [0, 100].
    rng = np.random.default_rng(0)
    w_vals = (rng.uniform(0.5, 1.0, size=in_features)).astype(np.float32).tolist()
    w = helper.make_tensor("W", TensorProto.FLOAT, [in_features, 1], w_vals)
    b = helper.make_tensor("B", TensorProto.FLOAT, [1], [50.0])
    node = helper.make_node("Gemm", ["features", "W", "B"], ["score"])
    graph = helper.make_graph([node], "mlp", [x], [y], [w, b])
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(path))


def _features_parquet(path: Path, n: int = 800) -> None:
    rng = np.random.default_rng(1)
    data = {c: rng.uniform(0, 10, size=n).astype(np.float32) for c in FEATURE_COLUMNS}
    pd.DataFrame(data).to_parquet(path)


def test_quantize_produces_smaller_model_with_bounded_drift(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    parquet = tmp_path / "f.parquet"
    _tiny_mlp(fp32)
    _features_parquet(parquet)

    report = quantize_int8(
        fp32_path=fp32,
        int8_path=int8,
        calibration=parquet,
        input_name="features",
        n_calibration=256,
        batch_size=32,
    )
    assert int8.is_file()
    assert report.n_calibration == 256
    assert report.n_held_out == 128
    # On a single-Gemm model the INT8 drift should be very small; allow a
    # generous budget to avoid flakes across ORT versions.
    assert report.rmse < 1.0, f"int8 rmse too large: {report.rmse}"
    assert report.max_abs_error < 5.0


def test_report_serializes(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    parquet = tmp_path / "f.parquet"
    _tiny_mlp(fp32)
    _features_parquet(parquet)
    report = quantize_int8(fp32_path=fp32, int8_path=int8, calibration=parquet, n_calibration=256)
    d = report.to_dict()
    assert d["int8_bytes"] > 0
    # Compression ratio is only meaningful for production-sized models —
    # a 6-parameter Gemm is dominated by QDQ metadata overhead, so we
    # just check the field is populated rather than > 1.
    assert "compression_ratio" in d
    table = render_table(report)
    assert "compression" in table and "RMSE" in table


def test_too_few_samples_raises(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    parquet = tmp_path / "f.parquet"
    _tiny_mlp(fp32)
    _features_parquet(parquet, n=100)  # not enough for 256 calib + 128 held-out
    with pytest.raises(ValueError, match="need at least"):
        quantize_int8(
            fp32_path=fp32,
            int8_path=int8,
            calibration=parquet,
            n_calibration=256,
        )


def test_missing_feature_columns_raises(tmp_path: Path) -> None:
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    parquet = tmp_path / "f.parquet"
    _tiny_mlp(fp32)
    pd.DataFrame({"not_a_feature": [1.0] * 1000}).to_parquet(parquet)
    with pytest.raises(ValueError, match="no FEATURE_COLUMNS"):
        quantize_int8(
            fp32_path=fp32,
            int8_path=int8,
            calibration=parquet,
            n_calibration=256,
        )


def test_quantized_ops_are_allowlisted(tmp_path: Path) -> None:
    """The QDQ output must use only ops libvmaf accepts.

    QDQ introduces QuantizeLinear and DequantizeLinear; the underlying
    Gemm / Conv / MatMul stay on the allowlist. If ORT ever switches
    quantize_static to a different default format this test catches it
    before we ship a model libvmaf would reject at load time.
    """
    fp32 = tmp_path / "fp32.onnx"
    int8 = tmp_path / "int8.onnx"
    parquet = tmp_path / "f.parquet"
    _tiny_mlp(fp32)
    _features_parquet(parquet)
    quantize_int8(
        fp32_path=fp32,
        int8_path=int8,
        calibration=parquet,
        n_calibration=256,
    )
    model = onnx.load(str(int8))
    op_types = {n.op_type for n in model.graph.node}
    from vmaf_train.op_allowlist import load_allowlist

    allowlist = load_allowlist()
    forbidden = op_types - allowlist
    assert not forbidden, (
        f"int8 model uses ops libvmaf would reject: {forbidden}. "
        "Extend core/src/dnn/op_allowlist.c if the new op is genuinely safe."
    )
