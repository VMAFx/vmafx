# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for the Research-0026 Phase 2 correlation analyser.

The harness lives at ``ai/scripts/feature_correlation.py``. These
tests cover the analytic functions on a synthetic parquet (no
libvmaf dependency) so the analysis pipeline can be verified
without a multi-hour full-feature extraction pass.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
from feature_correlation import _pearson_matrix, _redundant_pairs, _top_k_consensus
from feature_correlation import main as corr_main


def _make_synthetic_parquet(path: Path, *, n: int = 500, seed: int = 0) -> Path:
    rng = np.random.default_rng(seed)
    base = rng.standard_normal(n)
    redundant = base + 0.001 * rng.standard_normal(n)  # |r| ≈ 1.0
    independent = rng.standard_normal(n)
    target = 0.7 * base + 0.2 * independent + 0.1 * rng.standard_normal(n)
    df = pd.DataFrame(
        {
            "source": ["clipA"] * n,
            "dis_basename": ["clipA_dis.yuv"] * n,
            "frame_index": np.arange(n),
            "feat_a": base.astype(np.float32),
            "feat_b_redundant": redundant.astype(np.float32),
            "feat_c_independent": independent.astype(np.float32),
            "vmaf": target.astype(np.float32),
        }
    )
    df.to_parquet(path)
    return path


def test_pearson_matrix_diagonal_is_one():
    x = np.array([[1.0, 2.0], [3.0, 4.0], [5.0, 6.0]])
    m = _pearson_matrix(x, ["a", "b"])
    assert m["a"]["a"] == pytest.approx(1.0, abs=1e-9)
    assert m["b"]["b"] == pytest.approx(1.0, abs=1e-9)


def test_redundant_pairs_flags_near_perfect_correlation(tmp_path):
    parquet = _make_synthetic_parquet(tmp_path / "syn.parquet")
    df = pd.read_parquet(parquet)
    feat_cols = ["feat_a", "feat_b_redundant", "feat_c_independent"]
    x = df[feat_cols].to_numpy(dtype=np.float64)
    pairs = _redundant_pairs(x, feat_cols, threshold=0.9)
    assert len(pairs) == 1
    assert {pairs[0]["a"], pairs[0]["b"]} == {"feat_a", "feat_b_redundant"}
    assert pairs[0]["r"] > 0.99


def test_top_k_consensus_intersection():
    importances = {
        "mi": {"a": 1.0, "b": 0.5, "c": 0.1},
        "lasso": {"a": 0.9, "b": 0.4, "c": 0.0},
        "rf": {"a": 0.8, "c": 0.3, "b": 0.2},
    }
    consensus = _top_k_consensus(importances, k=2)
    assert "a" in consensus  # ranked top-2 by all three


def test_top_k_consensus_handles_missing_method():
    importances = {
        "mi": {"a": float("nan"), "b": float("nan")},
        "lasso": {"a": 1.0, "b": 0.0},
    }
    # mi is all-NaN → effectively dropped; lasso top-1 = "a"
    consensus = _top_k_consensus(importances, k=1)
    assert consensus == ["a"]


def test_corr_main_invokable_via_argparse(tmp_path, monkeypatch):
    pytest.importorskip("sklearn")
    parquet = _make_synthetic_parquet(tmp_path / "syn.parquet")
    out = tmp_path / "report.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out), "--top-k", "1"],
    )
    rc = corr_main()
    assert rc == 0
    payload = json.loads(out.read_text())
    assert "pearson" in payload
    assert "consensus_topk" in payload
    assert "redundant_pairs" in payload
    assert payload["run_provenance"]["schema"] == "ai-run-provenance-v1"
    assert payload["run_provenance"]["entrypoint"]["path"] == "ai/scripts/feature_correlation.py"
    assert payload["run_provenance"]["inputs"]["parquet"]["kind"] == "file"
    assert payload["run_provenance"]["outputs"]["json_report"]["path"] == str(out)
    assert payload["target"] == "vmaf"
    # The synthetic redundant pair must be flagged.
    redundant_names = {frozenset({p["a"], p["b"]}) for p in payload["redundant_pairs"]}
    assert frozenset({"feat_a", "feat_b_redundant"}) in redundant_names
    # Each method must have produced a top-K list for the registered features.
    for method in ("mi", "lasso", "rf"):
        assert method in payload["per_method_topk"]
