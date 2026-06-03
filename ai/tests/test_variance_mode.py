# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Variance-mode smoke tests — FRRegressor / NRMetric emit (score, logvar).

These exercise the architectural change only; we are not training the
models here, just checking:

  1. Default mode still returns a (N,) tensor (back-compat).
  2. emit_variance=True returns (N, 2).
  3. A single optimizer step on NLL produces a finite loss.
  4. ONNX export of a variance-mode model produces a (N, 2) output.
"""

from __future__ import annotations

from pathlib import Path

import pytest

torch = pytest.importorskip("torch")

from conftest import requires_pytorch_lightning  # noqa: E402

requires_pytorch_lightning()

import numpy as np  # noqa: E402


def test_fr_default_mode_unchanged() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6).eval()
    x = torch.randn(4, 6)
    with torch.no_grad():
        y = m(x)
    assert y.shape == (4,)


def test_fr_variance_mode_two_columns() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, emit_variance=True).eval()
    x = torch.randn(4, 6)
    with torch.no_grad():
        y = m(x)
    assert y.shape == (4, 2)


def test_fr_variance_training_step_finite() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, emit_variance=True)
    x = torch.randn(8, 6)
    y = torch.randn(8) * 20 + 50  # realistic MOS range
    opt = torch.optim.SGD(m.parameters(), lr=1e-3)
    # Lightning's training_step returns the loss; call it directly.
    loss = m._step((x, y), "train")
    assert torch.isfinite(loss)
    loss.backward()
    opt.step()


def test_nr_variance_mode_two_columns() -> None:
    from vmaf_train.models import NRMetric

    m = NRMetric(in_channels=1, width=8, emit_variance=True).eval()
    x = torch.randn(2, 1, 64, 64)
    with torch.no_grad():
        y = m(x)
    assert y.shape == (2, 2)


def test_fr_variance_onnx_export(tmp_path: Path) -> None:
    pytest.importorskip("onnx")
    ort = pytest.importorskip("onnxruntime")
    from vmaf_train.models import FRRegressor, export_to_onnx

    m = FRRegressor(in_features=6, emit_variance=True).eval()
    out_path = tmp_path / "fr_var.onnx"
    # The exporter treats the output as a single tensor — variance mode
    # produces a (N, 2) single tensor, so no special casing is needed.
    export_to_onnx(
        m,
        out_path,
        in_shape=(1, 6),
        input_name="features",
        output_name="score_logvar",
        atol=1e-5,
    )
    sess = ort.InferenceSession(str(out_path), providers=["CPUExecutionProvider"])
    x = np.random.default_rng(0).standard_normal((4, 6)).astype(np.float32)
    out = sess.run(None, {"features": x})[0]
    assert out.shape == (4, 2)
