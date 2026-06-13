# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke test for ``ai/train/train.py --epochs 0``.

Runs the training entry point as a subprocess against the mock corpus
fixture; asserts the script exits 0 and writes an ONNX file. This is
the CI-friendly version of the manual smoke-run command documented in
the PR template.
"""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path

import pytest

pytest.importorskip("torch")
pytest.importorskip("onnx")


REPO_ROOT = Path(__file__).resolve().parents[2]
TRAIN_SCRIPT = REPO_ROOT / "ai" / "train" / "train.py"


def test_train_epochs_zero_smoke(mock_corpus: Path, tmp_path: Path) -> None:
    out_dir = tmp_path / "out"
    cmd = [
        sys.executable,
        str(TRAIN_SCRIPT),
        "--epochs",
        "0",
        "--data-root",
        str(mock_corpus),
        "--out-dir",
        str(out_dir),
        "--assume-dims",
        "16x16",
        "--val-source",
        "BetaSrc",
        "--model-arch",
        "mlp_small",
    ]
    proc = subprocess.run(cmd, capture_output=True, text=True, timeout=180, cwd=REPO_ROOT)
    assert (
        proc.returncode == 0
    ), f"smoke run exit={proc.returncode}\nstdout={proc.stdout}\nstderr={proc.stderr}"
    onnx_files = list(out_dir.glob("*.onnx"))
    assert onnx_files, f"no ONNX written; out_dir={list(out_dir.iterdir())}"


def test_train_arch_param_counts() -> None:
    """Sanity-check that ``count_params`` matches the documented numbers."""
    from ai.train.train import _build_model, count_params

    assert count_params(_build_model("linear", 6)) == 7  # 6 weights + 1 bias
    assert count_params(_build_model("mlp_small", 6)) == 257
    assert count_params(_build_model("mlp_medium", 6)) == 2561
