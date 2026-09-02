# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the bisect-cache fixture generator."""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import numpy as np
import onnx
import pandas as pd

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "build_bisect_cache.py"


def _load_script():
    spec = importlib.util.spec_from_file_location("build_bisect_cache", SCRIPT)
    assert spec is not None
    assert spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def test_real_feature_parquet_materialises_bisect_cache(tmp_path: Path) -> None:
    mod = _load_script()
    src = tmp_path / "real.parquet"
    df = pd.DataFrame(
        {
            "adm2": [0.8, 0.6, 0.7, None],
            "vif_scale0": [0.5, 0.3, 0.4, 0.9],
            "vif_scale1": [0.6, 0.4, 0.5, 0.9],
            "vif_scale2": [0.7, 0.5, 0.6, 0.9],
            "vif_scale3": [0.8, 0.6, 0.7, 0.9],
            "motion2": [1.0, 2.0, 3.0, 4.0],
            "dmos": [91.0, 77.0, 82.0, 20.0],
        }
    )
    df.to_parquet(src, engine="pyarrow", compression="zstd", index=False)

    out = tmp_path / "bisect"
    mod.regenerate(out, source_features=src, target_column="dmos")

    features = pd.read_parquet(out / "features.parquet")
    assert list(features.columns) == [*mod.DEFAULT_FEATURES, "mos"]
    assert features.shape == (3, 7)
    np.testing.assert_allclose(features["mos"].to_numpy(), [91.0, 77.0, 82.0])
    models = sorted((out / "models").glob("model_*.onnx"))
    assert len(models) == mod.N_MODELS
    model = onnx.load(str(models[0]))
    assert {init.name for init in model.graph.initializer} >= {"W", "b"}


def test_source_feature_validation_reports_missing_columns(tmp_path: Path) -> None:
    mod = _load_script()
    src = tmp_path / "bad.parquet"
    pd.DataFrame({"adm2": [1.0], "mos": [4.0]}).to_parquet(src, index=False)

    try:
        mod.load_source_features(src)
    except ValueError as exc:
        assert "missing required feature columns" in str(exc)
        assert "vif_scale0" in str(exc)
    else:
        raise AssertionError("missing columns should fail")


def test_regenerate_writes_replay_manifest(tmp_path: Path) -> None:
    mod = _load_script()
    out = tmp_path / "bisect"
    manifest = tmp_path / "bisect_manifest.json"

    rc = mod.main(["--out", str(out), "--manifest-out", str(manifest)])

    assert rc == 0
    payload = json.loads(manifest.read_text(encoding="utf-8"))
    assert payload["schema"] == "bisect-cache-manifest-v1"
    assert payload["mode"] == "regenerate"
    assert payload["status"] == "written"
    assert payload["artifact_counts"] == {"feature_parquets": 1, "onnx_models": mod.N_MODELS}
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
