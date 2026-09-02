# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Contract tests for `model/tiny/saliency_student_v1.onnx`.

Locks down four invariants that the C-side `feature_mobilesal.c`
extractor relies on:

1. Op-allowlist: every op in the graph is on
   `core/src/dnn/op_allowlist.c`.
2. Tensor-name contract: input named `input`, output named
   `saliency_map`, both with the expected NCHW rank-4 layout.
3. Dynamic axes: the model accepts arbitrary spatial sizes (H, W)
   without retracing.
4. Content-dependence: the model emits substantively different
   saliency means on flat-grey vs noisy input — i.e. it is not the
   placeholder constant ~0.5.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import onnxruntime as ort
import pytest

from vmaf_train.op_allowlist import check_model

REPO_ROOT = Path(__file__).resolve().parents[2]
ONNX_PATH = REPO_ROOT / "model" / "tiny" / "saliency_student_v1.onnx"


@pytest.fixture(scope="module")
def session() -> ort.InferenceSession:
    if not ONNX_PATH.is_file():
        pytest.skip(f"{ONNX_PATH} not present")
    return ort.InferenceSession(str(ONNX_PATH), providers=["CPUExecutionProvider"])


def test_ops_on_allowlist() -> None:
    if not ONNX_PATH.is_file():
        pytest.skip(f"{ONNX_PATH} not present")
    report = check_model(ONNX_PATH)
    assert report.ok, report.pretty()


def test_tensor_name_contract(session: ort.InferenceSession) -> None:
    inputs = session.get_inputs()
    outputs = session.get_outputs()
    assert len(inputs) == 1
    assert len(outputs) == 1
    assert inputs[0].name == "input"
    assert outputs[0].name == "saliency_map"
    # Rank-4 NCHW
    assert len(inputs[0].shape) == 4
    assert len(outputs[0].shape) == 4
    # Channel axes are static — 3 in, 1 out
    assert inputs[0].shape[1] == 3
    assert outputs[0].shape[1] == 1


@pytest.mark.parametrize("h,w", [(64, 64), (256, 256), (192, 320)])
def test_dynamic_axes(session: ort.InferenceSession, h: int, w: int) -> None:
    x = np.random.RandomState(0).randn(1, 3, h, w).astype(np.float32)
    y = session.run(["saliency_map"], {"input": x})[0]
    assert y.shape == (1, 1, h, w)
    # Sigmoid output stays in [0, 1]
    assert float(y.min()) >= 0.0
    assert float(y.max()) <= 1.0


def test_content_dependence(session: ort.InferenceSession) -> None:
    grey = np.full((1, 3, 256, 256), 0.0, dtype=np.float32)
    noise = np.random.RandomState(0).randn(1, 3, 256, 256).astype(np.float32) * 2.0
    y_grey = float(session.run(["saliency_map"], {"input": grey})[0].mean())
    y_noise = float(session.run(["saliency_map"], {"input": noise})[0].mean())
    # The mobilesal_placeholder_v0 (constant Conv+Sigmoid) emits ~0.5
    # for every input. saliency_student_v1 is content-dependent.
    assert abs(y_grey - y_noise) > 0.1, (
        f"saliency mean diff {abs(y_grey - y_noise):.4f} is too small — "
        "model may be the placeholder"
    )
