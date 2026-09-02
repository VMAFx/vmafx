# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance sidecar tests for FR regressor trainers."""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

from train_fr_regressor_v2 import _write_sidecar_and_registry as write_v2_sidecar  # noqa: E402
from train_fr_regressor_v3 import write_sidecar_and_registry as write_v3_sidecar  # noqa: E402


def _registry(path: Path) -> Path:
    path.write_text(json.dumps({"models": []}) + "\n", encoding="utf-8")
    return path


def _onnx(path: Path) -> Path:
    path.write_bytes(b"fixture-onnx")
    return path


def test_fr_regressor_v2_sidecar_records_run_provenance(tmp_path: Path) -> None:
    sidecar = tmp_path / "fr_regressor_v2.json"
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": "ai/scripts/train_fr_regressor_v2.py"},
        "argv": ["--smoke"],
        "args": {"smoke": True},
    }

    payload = write_v2_sidecar(
        onnx_path=_onnx(tmp_path / "fr_regressor_v2.onnx"),
        sidecar_path=sidecar,
        registry_path=_registry(tmp_path / "registry.json"),
        scaler={"feature_mean": [0.0] * 6, "feature_std": [1.0] * 6},
        in_sample={"plcc": 0.99, "srocc": 0.98, "rmse": 0.1},
        n_rows=12,
        smoke=True,
        run_provenance=provenance,
    )

    written = json.loads(sidecar.read_text(encoding="utf-8"))
    assert payload["run_provenance"] == provenance
    assert written["run_provenance"] == provenance
    assert written["training"]["smoke"] is True


def test_fr_regressor_v3_sidecar_records_run_provenance(tmp_path: Path) -> None:
    sidecar = tmp_path / "fr_regressor_v3.json"
    provenance = {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": "ai/scripts/train_fr_regressor_v3.py"},
        "argv": ["--corpus", "fixture.jsonl"],
        "args": {"corpus": "fixture.jsonl"},
    }
    corpus = tmp_path / "fixture.jsonl"
    corpus.write_text("{}\n", encoding="utf-8")

    write_v3_sidecar(
        onnx_path=_onnx(tmp_path / "fr_regressor_v3.onnx"),
        sidecar_path=sidecar,
        registry_path=_registry(tmp_path / "registry.json"),
        scaler={"feature_mean": [0.0] * 6, "feature_std": [1.0] * 6},
        loso_summary={
            "mean_plcc": 0.99,
            "std_plcc": 0.01,
            "min_plcc": 0.98,
            "max_plcc": 1.0,
            "mean_srocc": 0.97,
            "mean_rmse": 0.2,
            "folds": [],
        },
        corpus_path=corpus,
        n_rows=12,
        smoke=False,
        gate_passed=True,
        run_provenance=provenance,
    )

    written = json.loads(sidecar.read_text(encoding="utf-8"))
    assert written["run_provenance"] == provenance
    assert written["gate_passed"] is True
    assert written["corpus_sha256"]
