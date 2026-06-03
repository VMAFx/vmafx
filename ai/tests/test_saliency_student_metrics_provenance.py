# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance tests for saliency-student training metrics."""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import pytest

torch = pytest.importorskip("torch")

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

import train_saliency_student as train_v1  # noqa: E402
import train_saliency_student_v2 as train_v2  # noqa: E402


def _args(tmp_path: Path) -> argparse.Namespace:
    return argparse.Namespace(
        duts_root=tmp_path / "DUTS-TR",
        output=tmp_path / "saliency.onnx",
        epochs=1,
        batch_size=2,
        lr=1e-3,
        crop_size=64,
        val_fraction=0.05,
        num_workers=0,
        seed=42,
        opset=17,
        device="cpu",
        metrics_out=tmp_path / "metrics.json",
    )


@pytest.mark.parametrize("module", [train_v1, train_v2])
def test_saliency_metrics_payload_records_run_provenance(tmp_path: Path, module) -> None:
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": f"ai/scripts/{module.__name__}.py"},
        "argv": ["--metrics-out", "metrics.json"],
        "args": {"metrics_out": "metrics.json"},
    }

    payload = module._build_metrics_payload(
        best_val_iou=0.75,
        param_count=123,
        args=_args(tmp_path),
        history=[{"epoch": 1, "val_iou": 0.75}],
        total_time_sec=2.5,
        onnx_bytes=456,
        onnx_sha256="abc123",
        pt_onnx_max_abs_diff=1e-6,
        device=torch.device("cpu"),
        run_provenance=provenance,
    )

    assert payload["best_val_iou"] == 0.75
    assert payload["onnx_sha256"] == "abc123"
    assert payload["run_provenance"] == provenance
