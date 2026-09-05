# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the PTQ harness (ADR-0173 / T5-3).

These don't run a full quantisation round-trip — that needs
`onnxruntime.quantization` installed and a real ONNX file. They check
that the script entry-points import cleanly and surface useful CLI help.
"""

from __future__ import annotations

import importlib.util
import json
import subprocess
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPTS_DIR = REPO_ROOT / "ai" / "scripts"


def _run_help(script: Path) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [sys.executable, str(script), "--help"],
        check=False,
        capture_output=True,
        text=True,
        timeout=15,
    )


def test_ptq_dynamic_help() -> None:
    cp = _run_help(SCRIPTS_DIR / "ptq_dynamic.py")
    assert cp.returncode == 0, cp.stderr
    assert "dynamic" in cp.stdout.lower()
    assert "--report-out" in cp.stdout
    # No `--calibration` flag — dynamic PTQ doesn't need one.
    assert "--calibration" not in cp.stdout


def test_ptq_static_help() -> None:
    cp = _run_help(SCRIPTS_DIR / "ptq_static.py")
    assert cp.returncode == 0, cp.stderr
    assert "calibration" in cp.stdout.lower()
    assert "--report-out" in cp.stdout


def test_qat_train_help() -> None:
    cp = _run_help(SCRIPTS_DIR / "qat_train.py")
    assert cp.returncode == 0, cp.stderr
    assert "config" in cp.stdout.lower()
    assert "--report-out" in cp.stdout


def test_ptq_dynamic_imports_only_what_it_needs() -> None:
    """Import the module without executing main() — must not require
    onnxruntime.quantization just to load the file."""
    spec = importlib.util.spec_from_file_location(
        "ptq_dynamic_under_test", SCRIPTS_DIR / "ptq_dynamic.py"
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    assert callable(mod.main)


@pytest.mark.skipif(
    importlib.util.find_spec("onnxruntime") is None,
    reason="onnxruntime not installed; skipping full ptq_dynamic round-trip",
)
def test_ptq_dynamic_full_roundtrip(tmp_path: Path) -> None:
    """End-to-end: build a tiny ONNX, quantise it, confirm the output
    file exists and is smaller than (or equal to) the input."""
    pytest.importorskip("onnxruntime.quantization")

    import numpy as np
    import onnx
    from onnx import TensorProto, helper

    src = tmp_path / "tiny.onnx"
    dst = tmp_path / "tiny.int8.onnx"
    report = tmp_path / "ptq_dynamic_report.json"

    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 4])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 4])
    weight = helper.make_tensor(
        "w",
        TensorProto.FLOAT,
        [4, 4],
        np.arange(16, dtype=np.float32).tolist(),
    )
    node = helper.make_node("MatMul", ["x", "w"], ["y"])
    graph = helper.make_graph([node], "g", [x], [y], initializer=[weight])
    onnx.save(
        helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)]),
        str(src),
    )

    cp = subprocess.run(
        [
            sys.executable,
            str(SCRIPTS_DIR / "ptq_dynamic.py"),
            str(src),
            "--output",
            str(dst),
            "--report-out",
            str(report),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert cp.returncode == 0, cp.stderr
    assert dst.is_file()
    # int8 quantised version is typically smaller; allow equal on tiny graphs
    # where the QDQ overhead dominates.
    assert dst.stat().st_size <= src.stat().st_size + 4096
    payload = json.loads(report.read_text())
    assert payload["mode"] == "dynamic"
    assert payload["output_bytes"] == dst.stat().st_size
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


# Allowlist copied from core/src/dnn/op_allowlist.c:101-105 + input graph ops
OP_ALLOWLIST_STATIC = {
    # op_allowlist.c:101-105
    "QuantizeLinear",
    "DequantizeLinear",
    "DynamicQuantizeLinear",
    "MatMulInteger",
    "ConvInteger",
    # Input graph ops
    "Conv",
    "Gemm",
    "MatMul",
    "Relu",
    "Reshape",
    "Constant",
}


@pytest.mark.skipif(
    importlib.util.find_spec("onnxruntime") is None or importlib.util.find_spec("onnx") is None,
    reason="onnxruntime or onnx not installed; skipping full ptq_static round-trip",
)
def test_ptq_static_full_roundtrip(tmp_path: Path) -> None:
    """End-to-end static PTQ: build a tiny Conv+Gemm ONNX, calibrate with .npz,
    and assert output is in QDQ format (all node op_types within allowlist,
    no QLinear* or QGemm ops). Proof test: fails if format ever flips to QOperator."""
    pytest.importorskip("onnxruntime.quantization")

    import numpy as np
    import onnx
    from onnx import TensorProto, helper

    src = tmp_path / "tiny_conv_gemm.onnx"
    cal = tmp_path / "calibration.npz"
    dst = tmp_path / "tiny_conv_gemm.int8.onnx"
    report = tmp_path / "ptq_static_report.json"

    x = helper.make_tensor_value_info("x", TensorProto.FLOAT, [1, 1, 4, 4])
    y = helper.make_tensor_value_info("y", TensorProto.FLOAT, [1, 2])

    w_conv = helper.make_tensor(
        "w_conv",
        TensorProto.FLOAT,
        [1, 1, 3, 3],
        np.ones((1, 1, 3, 3), dtype=np.float32).flatten().tolist(),
    )
    b_conv = helper.make_tensor("b_conv", TensorProto.FLOAT, [1], [0.0])
    conv = helper.make_node("Conv", ["x", "w_conv", "b_conv"], ["conv_out"], kernel_shape=[3, 3])
    relu = helper.make_node("Relu", ["conv_out"], ["relu_out"])

    shape_const = helper.make_tensor("shape_const", TensorProto.INT64, [2], [1, 4])
    reshape = helper.make_node("Reshape", ["relu_out", "shape_const"], ["reshaped"])

    w_gemm = helper.make_tensor(
        "w_gemm",
        TensorProto.FLOAT,
        [4, 2],
        np.ones((4, 2), dtype=np.float32).flatten().tolist(),
    )
    b_gemm = helper.make_tensor("b_gemm", TensorProto.FLOAT, [2], [0.0, 0.0])
    gemm = helper.make_node("Gemm", ["reshaped", "w_gemm", "b_gemm"], ["y"])

    graph = helper.make_graph(
        [conv, relu, reshape, gemm],
        "g",
        [x],
        [y],
        initializer=[w_conv, b_conv, shape_const, w_gemm, b_gemm],
    )
    model = helper.make_model(graph, opset_imports=[helper.make_opsetid("", 17)])
    onnx.save(model, str(src))

    cal_data = np.random.RandomState(42).randn(4, 1, 4, 4).astype(np.float32)
    np.savez(cal, x=cal_data)

    cp = subprocess.run(
        [
            sys.executable,
            str(SCRIPTS_DIR / "ptq_static.py"),
            str(src),
            "--calibration",
            str(cal),
            "--output",
            str(dst),
            "--report-out",
            str(report),
        ],
        check=False,
        capture_output=True,
        text=True,
        timeout=60,
    )
    assert cp.returncode == 0, cp.stderr
    assert dst.is_file()

    quantized_model = onnx.load(str(dst))
    op_types = [node.op_type for node in quantized_model.graph.node]
    assert len(op_types) > 0

    for op in op_types:
        assert op in OP_ALLOWLIST_STATIC, f"Op {op} not in allowlist"
        assert not op.startswith("QLinear"), f"Unexpected QOperator op: {op}"
        assert not op.startswith("QGemm"), f"Unexpected QOperator op: {op}"

    payload = json.loads(report.read_text())
    assert payload["mode"] == "static"
    assert payload["output_bytes"] == dst.stat().st_size
    assert payload["schema"] == "ptq-static-report-v1"
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"


def test_measure_quant_drop_report_for_fp32_skip(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The CI quant-drop gate can write a provenance-backed report without ORT."""
    spec = importlib.util.spec_from_file_location(
        "measure_quant_drop_under_test", SCRIPTS_DIR / "measure_quant_drop.py"
    )
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)

    repo = tmp_path / "repo"
    registry = repo / "model" / "tiny" / "registry.json"
    registry.parent.mkdir(parents=True)
    registry.write_text(
        json.dumps(
            {
                "models": [
                    {
                        "id": "fp32_model",
                        "onnx": "fp32_model.onnx",
                        "quant_mode": "fp32",
                    }
                ]
            }
        )
        + "\n"
    )
    report = tmp_path / "quant_drop_report.json"

    monkeypatch.setattr(mod, "REPO_ROOT", repo)
    monkeypatch.setattr(mod, "REGISTRY", registry)

    assert mod.main(["--all", "--out-json", str(report)]) == 0
    payload = json.loads(report.read_text())
    assert payload["gate_pass"] is True
    assert payload["models"][0]["status"] == "skipped_fp32"
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
