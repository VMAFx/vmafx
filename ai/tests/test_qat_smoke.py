# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke test for ``ai/scripts/qat_train.py --smoke`` and the
``ai.train.qat.run_qat`` Python API.

Exercises the QAT pipeline with zero training epochs so the test
runs in seconds — the goal is to catch wiring breakage (FX trace,
weight transfer, ONNX export, ORT static-quantize round-trip),
not to validate training accuracy. Mirrors the ``--epochs 0``
smoke pattern in ``ai/train/train.py``.

Test plan (per ADR-0207):

* :func:`test_qat_run_smoke` — direct Python call to
  ``run_qat`` against a fresh ``LearnedFilter``; verifies the
  resulting ``.int8.onnx`` loads under ORT CPU EP.
* :func:`test_qat_train_cli_smoke` — invokes the CLI driver as a
  subprocess with ``--smoke`` so ``argparse`` + config wiring
  also lands in CI coverage.
"""

from __future__ import annotations

import json
import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("torch")
pytest.importorskip("onnx")
pytest.importorskip("onnxruntime")
pytest.importorskip("yaml")

from conftest import requires_pytorch_lightning

requires_pytorch_lightning()

REPO_ROOT = Path(__file__).resolve().parents[2]
QAT_SCRIPT = REPO_ROOT / "ai" / "scripts" / "qat_train.py"
QAT_CONFIG = REPO_ROOT / "ai" / "configs" / "learned_filter_v1_qat.yaml"


def test_qat_run_smoke(tmp_path: Path) -> None:
    """Direct API: run_qat with smoke=True must land an int8 ONNX."""
    import sys as _sys

    if str(REPO_ROOT / "ai" / "src") not in _sys.path:
        _sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
    if str(REPO_ROOT) not in _sys.path:
        _sys.path.insert(0, str(REPO_ROOT))

    import numpy as np
    import onnxruntime as ort

    from ai.src.vmaf_train.models import LearnedFilter
    from ai.train.qat import QatConfig, run_qat

    int8_path = tmp_path / "smoke.int8.onnx"
    cfg = QatConfig(
        epochs_fp32=0,
        epochs_qat=0,
        n_calibration=4,
        output_int8_onnx=int8_path,
        smoke=True,
    )

    def factory():
        return LearnedFilter(channels=1, width=8, num_blocks=2)

    result = run_qat(
        model_factory=factory,
        qat_cfg=cfg,
        input_names=["degraded"],
        output_names=["filtered"],
        dynamic_axes={"degraded": {0: "batch"}, "filtered": {0: "batch"}},
        train_loader_factory=None,
    )
    assert result.int8_onnx.is_file(), "int8 ONNX was not produced"
    assert result.fp32_onnx.is_file(), "intermediate fp32 ONNX was not produced"
    assert result.int8_onnx.stat().st_size > 0
    assert result.n_params > 0

    # Round-trip the int8 model on ORT CPU EP — catches the
    # quantize_static-emits-an-unloadable-graph class of bug.
    sess = ort.InferenceSession(str(int8_path), providers=["CPUExecutionProvider"])
    out = sess.run(
        None,
        {"degraded": np.zeros((1, 1, 32, 32), dtype=np.float32)},
    )
    assert out[0].shape == (1, 1, 32, 32)


def test_qat_train_cli_smoke(tmp_path: Path) -> None:
    """CLI: ``qat_train.py --smoke`` exits 0 and writes the int8 ONNX."""
    int8_path = tmp_path / "out.int8.onnx"
    report_path = tmp_path / "qat_report.json"
    cmd = [
        sys.executable,
        str(QAT_SCRIPT),
        "--config",
        str(QAT_CONFIG),
        "--output",
        str(int8_path),
        "--epochs-fp32",
        "0",
        "--epochs-qat",
        "0",
        "--n-calibration",
        "4",
        "--smoke",
        "--report-out",
        str(report_path),
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=300, cwd=REPO_ROOT)
    assert (
        proc.returncode == 0
    ), f"qat_train smoke failed: stdout={proc.stdout}\nstderr={proc.stderr}"
    assert int8_path.is_file(), f"no int8 ONNX written; tmp={list(tmp_path.iterdir())}"
    report = json.loads(report_path.read_text())
    assert report["mode"] == "qat"
    assert report["int8_onnx"] == str(int8_path.resolve())
    assert report["run_provenance"]["schema"] == "ai-run-provenance-v1"
