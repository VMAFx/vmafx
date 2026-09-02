# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for Phase-3 subset sweep report wiring."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pandas as pd

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
import phase3_subset_sweep


def test_phase3_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    parquet = tmp_path / "features.parquet"
    out = tmp_path / "phase3.json"
    rows = []
    for source in ("clip-a", "clip-b"):
        for idx in range(3):
            rows.append(
                {
                    "source": source,
                    "adm2": 0.1 + idx,
                    "vif_scale0": 0.2 + idx,
                    "vif_scale1": 0.3 + idx,
                    "vif_scale2": 0.4 + idx,
                    "vif_scale3": 0.5 + idx,
                    "motion2": 0.6 + idx,
                    "vmaf": 70.0 + idx,
                }
            )
    pd.DataFrame(rows).to_parquet(parquet)

    def fake_loso_sweep(*_args, **_kwargs):
        return {
            "clip-a": {"plcc": 0.91, "srocc": 0.9, "rmse": 0.2},
            "clip-b": {"plcc": 0.93, "srocc": 0.92, "rmse": 0.3},
        }

    monkeypatch.setattr(phase3_subset_sweep, "_loso_sweep", fake_loso_sweep)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "phase3_subset_sweep.py",
            "--parquet",
            str(parquet),
            "--out",
            str(out),
            "--subsets",
            "canonical6",
            "--epochs",
            "1",
        ],
    )

    assert phase3_subset_sweep.main() == 0

    payload = json.loads(out.read_text(encoding="utf-8"))
    assert "canonical6" in payload
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == "ai/scripts/phase3_subset_sweep.py"
    assert provenance["inputs"]["parquet"]["kind"] == "file"
    assert provenance["outputs"]["json_report"]["path"] == str(out)
    assert provenance["args"]["subsets"] == "canonical6"
