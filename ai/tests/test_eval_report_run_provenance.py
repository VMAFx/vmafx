# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance report tests for tiny-VMAF eval scripts."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from types import ModuleType
from typing import Any

import pandas as pd

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

from ai.scripts import eval_loso_vmaf_tiny_v3 as eval_v3  # noqa: E402
from ai.scripts import eval_loso_vmaf_tiny_v4 as eval_v4  # noqa: E402
from ai.scripts import eval_loso_vmaf_tiny_v5 as eval_v5  # noqa: E402
from ai.scripts import eval_multiseed_v3_v4 as eval_ms  # noqa: E402

FEATURES = ("adm2", "vif_scale0", "vif_scale1", "vif_scale2", "vif_scale3", "motion2")


def _write_parquet(path: Path, *, with_corpus: bool = False) -> Path:
    rows: list[dict[str, Any]] = []
    for source_idx, source in enumerate(("src_a", "src_b")):
        for row_idx in range(3):
            row = {
                feature: float(source_idx + row_idx + idx) for idx, feature in enumerate(FEATURES)
            }
            row["source"] = source
            row["vmaf"] = float(80 + source_idx + row_idx)
            if with_corpus:
                row["corpus"] = "netflix"
            rows.append(row)
    pd.DataFrame(rows).to_parquet(path)
    return path


def _stub_fold_metrics(_model, _mean, _std, x_val, _y_val) -> dict[str, float]:  # type: ignore[no-untyped-def]
    return {"n": len(x_val), "plcc": 0.99, "srocc": 0.98, "rmse": 0.1}


def _assert_report(path: Path, entrypoint: str, input_key: str = "parquet") -> None:
    report = json.loads(path.read_text(encoding="utf-8"))
    provenance = report["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == entrypoint
    assert provenance["inputs"][input_key]["kind"] == "file"
    assert provenance["outputs"]["report_target"] == str(path)


def _run_single_loso_script(monkeypatch, tmp_path: Path, module: ModuleType, name: str) -> None:
    parquet = _write_parquet(tmp_path / f"{name}.parquet")
    out_json = tmp_path / f"{name}.json"
    monkeypatch.setattr(module, "_train", lambda *_args, **_kwargs: object())
    monkeypatch.setattr(module, "_eval_fold", _stub_fold_metrics)
    monkeypatch.setattr(
        sys,
        "argv",
        [f"{name}.py", "--parquet", str(parquet), "--out-json", str(out_json), "--epochs", "0"],
    )

    assert module.main() == 0
    _assert_report(out_json, f"ai/scripts/{name}.py")


def test_vmaf_tiny_v3_loso_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    _run_single_loso_script(monkeypatch, tmp_path, eval_v3, "eval_loso_vmaf_tiny_v3")


def test_vmaf_tiny_v4_loso_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    _run_single_loso_script(monkeypatch, tmp_path, eval_v4, "eval_loso_vmaf_tiny_v4")


def test_multiseed_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    parquet = _write_parquet(tmp_path / "multiseed.parquet")
    out_json = tmp_path / "multiseed.json"

    def fake_run_one_seed(_df, *, sources, **_kwargs):  # type: ignore[no-untyped-def]
        return {source: {"n": 3, "plcc": 0.99, "srocc": 0.98, "rmse": 0.1} for source in sources}

    monkeypatch.setattr(eval_ms, "_run_one_seed", fake_run_one_seed)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_multiseed_v3_v4.py",
            "--arch",
            "mlp_medium",
            "--parquet",
            str(parquet),
            "--out-json",
            str(out_json),
            "--seeds",
            "0",
        ],
    )

    assert eval_ms.main() == 0
    _assert_report(out_json, "ai/scripts/eval_multiseed_v3_v4.py")


def test_vmaf_tiny_v5_report_records_run_provenance(monkeypatch, tmp_path: Path) -> None:
    base = _write_parquet(tmp_path / "base.parquet", with_corpus=True)
    extra = _write_parquet(tmp_path / "extra.parquet", with_corpus=True)
    out_json = tmp_path / "v5.json"

    def fake_run_loso(_df, label: str, *_args) -> dict[str, Any]:
        mean_plcc = 0.99 if label == "v5" else 0.97
        return {
            "per_fold": {"src_a": {"n": 3, "plcc": mean_plcc, "srocc": 0.98, "rmse": 0.1}},
            "aggregate": {
                "mean_plcc": mean_plcc,
                "std_plcc": 0.01,
                "mean_srocc": 0.98,
                "std_srocc": 0.01,
                "mean_rmse": 0.1,
                "std_rmse": 0.01,
            },
        }

    monkeypatch.setattr(eval_v5, "_run_loso", fake_run_loso)
    monkeypatch.setattr(
        sys,
        "argv",
        [
            "eval_loso_vmaf_tiny_v5.py",
            "--parquet-base",
            str(base),
            "--parquet-extra",
            str(extra),
            "--out-json",
            str(out_json),
            "--epochs",
            "0",
        ],
    )

    assert eval_v5.main() == 0
    _assert_report(out_json, "ai/scripts/eval_loso_vmaf_tiny_v5.py", "parquet_base")
    report = json.loads(out_json.read_text(encoding="utf-8"))
    assert report["run_provenance"]["inputs"]["parquet_extra"]["kind"] == "file"
