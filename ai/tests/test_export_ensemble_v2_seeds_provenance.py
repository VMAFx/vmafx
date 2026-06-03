# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for ensemble seed export sidecar provenance."""

from __future__ import annotations

import importlib.util
import sys
from pathlib import Path

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "export_ensemble_v2_seeds.py"
_SPEC = importlib.util.spec_from_file_location("export_ensemble_v2_seeds", _SCRIPT)
assert _SPEC is not None and _SPEC.loader is not None
export_ensemble = importlib.util.module_from_spec(_SPEC)
sys.modules["export_ensemble_v2_seeds"] = export_ensemble
_SPEC.loader.exec_module(export_ensemble)


def _sidecar(*, run_provenance: dict | None = None) -> dict:
    promote = {
        "verdict": "PROMOTE",
        "gate": {
            "mean_plcc": 0.99,
            "mean_plcc_threshold": 0.95,
            "plcc_spread": 0.001,
            "plcc_spread_max": 0.005,
            "per_seed_min": 0.95,
            "per_seed_plccs": {"0": 0.991},
            "passed": True,
        },
    }
    return export_ensemble._build_sidecar(
        0,
        onnx_name="fr_regressor_v2_ensemble_v1_seed0.onnx",
        onnx_sha256="0" * 64,
        feature_mean=[0.0] * len(export_ensemble.CANONICAL_6),
        feature_std=[1.0] * len(export_ensemble.CANONICAL_6),
        cq_min=18.0,
        cq_max=34.0,
        n_rows=10,
        epochs=1,
        batch_size=2,
        lr=0.001,
        weight_decay=0.0,
        corpus_path="runs/corpus.jsonl",
        corpus_sha256="1" * 64,
        promote=promote,
        run_provenance=run_provenance,
    )


def test_build_sidecar_records_run_provenance_when_supplied() -> None:
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": "ai/scripts/export_ensemble_v2_seeds.py"},
    }

    sidecar = _sidecar(run_provenance=provenance)

    assert sidecar["run_provenance"] == provenance


def test_build_sidecar_omits_run_provenance_when_not_supplied() -> None:
    assert "run_provenance" not in _sidecar()
