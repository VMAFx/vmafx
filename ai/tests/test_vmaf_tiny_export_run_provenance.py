# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance sidecar tests for vmaf_tiny v2/v3/v4 exporters."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

from export_vmaf_tiny_v2 import _write_sidecar as write_v2_sidecar  # noqa: E402
from export_vmaf_tiny_v3 import _write_sidecar as write_v3_sidecar  # noqa: E402
from export_vmaf_tiny_v4 import _write_sidecar as write_v4_sidecar  # noqa: E402

FEATURES = ["adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2"]


def _provenance(entrypoint: str) -> dict[str, object]:
    return {
        "schema": "ai-run-provenance-v1",
        "entrypoint": {"path": entrypoint},
        "argv": ["--ckpt", "fixture.pt"],
        "args": {"ckpt": "fixture.pt"},
        "inputs": {"checkpoint": {"path": "fixture.pt", "kind": "missing"}},
    }


def _assert_written_payload(
    path: Path, payload: dict[str, object], provenance: dict[str, object]
) -> None:
    written = json.loads(path.read_text(encoding="utf-8"))
    assert written == payload
    assert written["run_provenance"] == provenance
    assert written["input_name"] == "features"
    assert written["output_name"] == "vmaf"
    assert written["features"] == FEATURES


def test_vmaf_tiny_v2_export_sidecar_records_run_provenance(tmp_path: Path) -> None:
    sidecar = tmp_path / "vmaf_tiny_v2.json"
    provenance = _provenance("ai/scripts/export_vmaf_tiny_v2.py")

    payload = write_v2_sidecar(
        sidecar_path=sidecar,
        onnx_name="vmaf_tiny_v2.onnx",
        digest="sha256-v2",
        in_dim=6,
        features=FEATURES,
        mean=np.zeros(6, dtype=np.float64),
        std=np.ones(6, dtype=np.float64),
        run_provenance=provenance,
    )

    _assert_written_payload(sidecar, payload, provenance)
    assert payload["id"] == "vmaf_tiny_v2"


def test_vmaf_tiny_v3_export_sidecar_records_run_provenance(tmp_path: Path) -> None:
    sidecar = tmp_path / "vmaf_tiny_v3.json"
    provenance = _provenance("ai/scripts/export_vmaf_tiny_v3.py")

    payload = write_v3_sidecar(
        sidecar_path=sidecar,
        onnx_name="vmaf_tiny_v3.onnx",
        digest="sha256-v3",
        in_dim=6,
        features=FEATURES,
        mean=np.zeros(6, dtype=np.float64),
        std=np.ones(6, dtype=np.float64),
        n_params=769,
        run_provenance=provenance,
    )

    _assert_written_payload(sidecar, payload, provenance)
    assert payload["id"] == "vmaf_tiny_v3"
    assert payload["arch"] == "mlp_medium"


def test_vmaf_tiny_v4_export_sidecar_records_run_provenance(tmp_path: Path) -> None:
    sidecar = tmp_path / "vmaf_tiny_v4.json"
    provenance = _provenance("ai/scripts/export_vmaf_tiny_v4.py")

    payload = write_v4_sidecar(
        sidecar_path=sidecar,
        onnx_name="vmaf_tiny_v4.onnx",
        digest="sha256-v4",
        in_dim=6,
        features=FEATURES,
        mean=np.zeros(6, dtype=np.float64),
        std=np.ones(6, dtype=np.float64),
        n_params=3073,
        run_provenance=provenance,
    )

    _assert_written_payload(sidecar, payload, provenance)
    assert payload["id"] == "vmaf_tiny_v4"
    assert payload["arch"] == "mlp_large"
