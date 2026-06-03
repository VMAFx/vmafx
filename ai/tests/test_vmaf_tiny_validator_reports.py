# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Report-provenance tests for tiny-VMAF validator CLIs."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

import validate_vmaf_tiny_v2 as validate_v2  # noqa: E402
import validate_vmaf_tiny_v3 as validate_v3  # noqa: E402
import validate_vmaf_tiny_v4 as validate_v4  # noqa: E402


def _write_feature_fixture(path: Path) -> None:
    adm2 = np.array([1.0, 2.0, 3.0, 4.0], dtype=np.float32)
    frame = pd.DataFrame(
        {
            "adm2": adm2,
            "vif_scale0": adm2 + 0.1,
            "vif_scale1": adm2 + 0.2,
            "vif_scale2": adm2 + 0.3,
            "vif_scale3": adm2 + 0.4,
            "motion2": adm2 + 0.5,
            "vmaf": adm2,
        }
    )
    frame.to_parquet(path)


@pytest.mark.parametrize(
    ("module", "model", "script"),
    [
        (validate_v2, "vmaf_tiny_v2", "ai/scripts/validate_vmaf_tiny_v2.py"),
        (validate_v3, "vmaf_tiny_v3", "ai/scripts/validate_vmaf_tiny_v3.py"),
        (validate_v4, "vmaf_tiny_v4", "ai/scripts/validate_vmaf_tiny_v4.py"),
    ],
)
def test_validator_out_json_records_run_provenance(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    module,
    model: str,
    script: str,
) -> None:
    parquet = tmp_path / "features.parquet"
    _write_feature_fixture(parquet)
    onnx = tmp_path / f"{model}.onnx"
    onnx.write_bytes(b"fixture")
    out_json = tmp_path / "report.json"

    def fake_predict(_onnx_path: Path, x: np.ndarray, _input_name: str) -> np.ndarray:
        return x[:, 0].astype(np.float32)

    monkeypatch.setattr(module, "_ort_predict", fake_predict)

    rc = module.main(
        [
            "--onnx",
            str(onnx),
            "--parquet",
            str(parquet),
            "--rows",
            "4",
            "--min-plcc",
            "0.99",
            "--out-json",
            str(out_json),
        ]
    )

    assert rc == 0
    report = json.loads(out_json.read_text(encoding="utf-8"))
    assert report["model"] == model
    assert report["gate_pass"] is True
    assert report["plcc"] > 0.99
    assert report["run_provenance"]["entrypoint"]["path"] == script
    assert report["run_provenance"]["inputs"]["parquet"]["sha256"]
    assert report["run_provenance"]["outputs"]["report"]["path"] == str(out_json)
