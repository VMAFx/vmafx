# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Run-provenance tests for vmaf_tiny training stats JSON files."""

from __future__ import annotations

import json
import sys
from pathlib import Path
from typing import Any

import numpy as np
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(_REPO_ROOT / "ai" / "scripts"))

import train_vmaf_tiny_v2 as train_v2  # noqa: E402
import train_vmaf_tiny_v3 as train_v3  # noqa: E402
import train_vmaf_tiny_v4 as train_v4  # noqa: E402
import train_vmaf_tiny_v5 as train_v5  # noqa: E402

CANONICAL_6 = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _write_feature_parquet(path: Path, *, corpus: str | None = None) -> None:
    import pandas as pd

    n_rows = 8
    data: dict[str, Any] = {
        name: np.linspace(0.1 + idx, 1.1 + idx, n_rows) for idx, name in enumerate(CANONICAL_6)
    }
    data["vmaf"] = np.linspace(70.0, 93.0, n_rows)
    if corpus is not None:
        data["corpus"] = [corpus] * n_rows
    pd.DataFrame(data).to_parquet(path)


def _assert_common_provenance(
    payload: dict[str, Any],
    *,
    script_name: str,
    argv: list[str],
    ckpt: Path,
    stats: Path,
) -> None:
    provenance = payload["run_provenance"]
    assert provenance["schema"] == "ai-run-provenance-v1"
    assert provenance["entrypoint"]["path"] == f"ai/scripts/{script_name}"
    assert provenance["argv"] == argv[1:]
    assert provenance["args"]["epochs"] == 0
    assert provenance["outputs"]["checkpoint_target"] == str(ckpt)
    assert provenance["outputs"]["stats_target"] == str(stats)


@pytest.mark.parametrize(
    ("module", "script_name"),
    [
        (train_v2, "train_vmaf_tiny_v2.py"),
        (train_v3, "train_vmaf_tiny_v3.py"),
        (train_v4, "train_vmaf_tiny_v4.py"),
    ],
)
def test_vmaf_tiny_single_parquet_train_stats_record_run_provenance(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    module: Any,
    script_name: str,
) -> None:
    parquet = tmp_path / "features.parquet"
    ckpt = tmp_path / "model.pt"
    stats = tmp_path / "stats.json"
    _write_feature_parquet(parquet)
    argv = [
        script_name,
        "--parquet",
        str(parquet),
        "--out-ckpt",
        str(ckpt),
        "--out-stats",
        str(stats),
        "--epochs",
        "0",
    ]
    monkeypatch.setattr(sys, "argv", argv)

    assert module.main() == 0

    payload = json.loads(stats.read_text(encoding="utf-8"))
    _assert_common_provenance(payload, script_name=script_name, argv=argv, ckpt=ckpt, stats=stats)
    assert payload["run_provenance"]["inputs"]["parquet"]["path"] == str(parquet)
    assert payload["run_provenance"]["inputs"]["parquet"]["kind"] == "file"


def test_vmaf_tiny_v5_train_stats_record_run_provenance(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    base = tmp_path / "base.parquet"
    extra = tmp_path / "extra.parquet"
    ckpt = tmp_path / "model.pt"
    stats = tmp_path / "stats.json"
    _write_feature_parquet(base, corpus="base")
    _write_feature_parquet(extra, corpus="ugc")
    argv = [
        "train_vmaf_tiny_v5.py",
        "--parquet-base",
        str(base),
        "--parquet-extra",
        str(extra),
        "--out-ckpt",
        str(ckpt),
        "--out-stats",
        str(stats),
        "--epochs",
        "0",
    ]
    monkeypatch.setattr(sys, "argv", argv)

    assert train_v5.main() == 0

    payload = json.loads(stats.read_text(encoding="utf-8"))
    _assert_common_provenance(
        payload,
        script_name="train_vmaf_tiny_v5.py",
        argv=argv,
        ckpt=ckpt,
        stats=stats,
    )
    provenance = payload["run_provenance"]
    assert provenance["inputs"]["parquet_base"]["path"] == str(base)
    assert provenance["inputs"]["parquet_extra"]["path"] == str(extra)
