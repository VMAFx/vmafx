# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Smoke tests for the Research-0026 Phase 2 correlation analyser.

The harness lives at ``ai/scripts/feature_correlation.py``. These
tests cover the analytic functions on a synthetic parquet (no
libvmaf dependency) so the analysis pipeline can be verified
without a multi-hour full-feature extraction pass.
"""

from __future__ import annotations

import builtins
import json
import sys
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))

# pylint: disable=wrong-import-position
from feature_correlation import (
    _parse_args,
    _pearson_matrix,
    _redundant_pairs,
    _select_feature_columns,
    _top_k_consensus,
    _write_strict_report,
)
from feature_correlation import (
    main as corr_main,
)

ARGPARSE_ERROR_CODE = 2


def _reject_nonfinite_json(token: str) -> None:
    raise AssertionError(f"non-finite JSON token: {token}")


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


def test_feature_selection_preserves_numeric_schema_order():
    df = pd.DataFrame(
        {
            "nullable_float": pd.Series([1.0, 2.0, None], dtype="Float64"),
            "flag": pd.Series([True, False, True], dtype="boolean"),
            "nullable_int": pd.Series([1, 2, None], dtype="Int64"),
            "category": pd.Series(["a", "b", "a"], dtype="category"),
            "all_nan": [np.nan, np.nan, np.nan],
            "constant": [2.0, 2.0, 2.0],
        }
    )

    usable, non_numeric, all_nan, constant = _select_feature_columns(df, list(df.columns))

    assert usable == ["nullable_float", "nullable_int"]
    assert non_numeric == ["category", "flag"]
    assert all_nan == ["all_nan"]
    assert constant == ["constant"]


def test_corr_main_skips_non_numeric_columns(tmp_path, monkeypatch, capsys):
    """Regression test: non-numeric columns (e.g. `codec` string) used to
    crash with `ValueError: could not convert string to float: 'x264'`.
    The fix selects numeric dtypes before to_numpy()."""
    pytest.importorskip("sklearn")
    parquet = tmp_path / "syn_with_string.parquet"
    rng = np.random.default_rng(0)
    n = 200
    base = rng.standard_normal(n)
    target = 0.7 * base + 0.1 * rng.standard_normal(n)
    df = pd.DataFrame(
        {
            "source": ["clipA"] * n,
            "dis_basename": ["clipA_dis.yuv"] * n,
            "frame_index": np.arange(n),
            "codec": rng.choice(["x264", "x265", "unknown"], size=n),  # non-numeric string
            "chug_orientation": ["landscape"] * n,  # non-numeric metadata
            "feat_a": base.astype(np.float32),
            "feat_b": rng.standard_normal(n).astype(np.float32),
            "vmaf": target.astype(np.float32),
        }
    )
    df.to_parquet(parquet)
    out = tmp_path / "report.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out), "--top-k", "1"],
    )
    rc = corr_main()
    assert rc == 0
    captured = capsys.readouterr().out
    assert "[corr] skipped non-numeric columns: ['chug_orientation', 'codec']" in captured
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    # non-numeric columns must be skipped; numeric features must be present
    feature_keys = set(payload.get("pearson", {}).keys())
    assert "feat_a" in feature_keys
    assert "feat_b" in feature_keys
    assert "codec" not in feature_keys
    assert "chug_orientation" not in feature_keys
    assert payload["feature_cols"] == ["feat_a", "feat_b"]
    assert payload["skipped_non_numeric_columns"] == ["chug_orientation", "codec"]
    assert payload["skipped_all_nan_columns"] == []
    assert payload["skipped_constant_columns"] == []


def test_corr_main_skips_all_nan_numeric_columns(tmp_path, monkeypatch, capsys):
    """Unavailable numeric features must not erase every complete-case row."""
    pytest.importorskip("sklearn")
    parquet = tmp_path / "syn_with_all_nan.parquet"
    rng = np.random.default_rng(1)
    n = 200
    feature = rng.standard_normal(n)
    target = 0.7 * feature + 0.1 * rng.standard_normal(n)
    pd.DataFrame(
        {
            "source": ["clipA"] * n,
            "frame_index": np.arange(n),
            "feat_available": feature.astype(np.float32),
            "feat_unavailable": np.full(n, np.nan, dtype=np.float32),
            "vmaf": target.astype(np.float32),
        }
    ).to_parquet(parquet)
    out = tmp_path / "report.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out)],
    )

    assert corr_main() == 0
    assert "[corr] skipped all-NaN numeric columns: ['feat_unavailable']" in capsys.readouterr().out
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    assert payload["feature_cols"] == ["feat_available"]
    assert payload["skipped_all_nan_columns"] == ["feat_unavailable"]
    assert "feat_unavailable" not in payload["pearson"]


def test_corr_main_skips_constant_numeric_columns(tmp_path, monkeypatch, capsys):
    """Zero-variance features must not warn or enter rankings as useful signal."""
    pytest.importorskip("sklearn")
    parquet = tmp_path / "syn_with_constant.parquet"
    feature = np.linspace(-1.0, 1.0, 200, dtype=np.float32)
    pd.DataFrame(
        {
            "feat_varying": feature,
            "feat_constant": np.full(feature.shape, 7.0, dtype=np.float32),
            "vmaf": 50.0 + feature,
        }
    ).to_parquet(parquet)
    out = tmp_path / "report.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out)],
    )

    assert corr_main() == 0
    assert "[corr] skipped constant numeric columns: ['feat_constant']" in capsys.readouterr().out
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    assert payload["feature_cols"] == ["feat_varying"]
    assert payload["skipped_constant_columns"] == ["feat_constant"]
    assert "feat_constant" not in payload["pearson"]
    assert "feat_constant" not in payload["consensus_topk"]


def test_corr_main_skips_feature_constant_only_after_complete_case(tmp_path, monkeypatch, capsys):
    """A feature can lose all variance after rows missing a sibling are dropped."""
    parquet = tmp_path / "syn_with_retained_constant.parquet"
    pd.DataFrame(
        {
            "feat_becomes_constant": [0.0] * 6 + [1.0, 2.0, 3.0, 4.0],
            "feat_varying": [1.0, 2.0, 3.0, 4.0, 5.0, 6.0] + [np.nan] * 4,
            "vmaf": np.linspace(70.0, 79.0, 10),
        }
    ).to_parquet(parquet)
    out = tmp_path / "report.json"
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out)],
    )

    assert corr_main() == 0
    assert (
        "[corr] skipped constant numeric columns: ['feat_becomes_constant']"
        in capsys.readouterr().out
    )
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    assert payload["feature_cols"] == ["feat_varying"]
    assert payload["skipped_constant_columns"] == ["feat_becomes_constant"]


def test_corr_main_without_sklearn_writes_strict_json(tmp_path, monkeypatch):
    """An unavailable optional analyser must not serialize NaN placeholders."""
    parquet = _make_synthetic_parquet(tmp_path / "syn.parquet")
    out = tmp_path / "report.json"
    real_import = builtins.__import__

    def import_without_sklearn(name, *args, **kwargs):
        if name == "sklearn" or name.startswith("sklearn."):
            raise ImportError("sklearn deliberately unavailable")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(builtins, "__import__", import_without_sklearn)
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(out)],
    )

    assert corr_main() == 0
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    assert payload["importances"] == {"mi": {}, "lasso": {}, "rf": {}}
    assert payload["per_method_topk"] == {"mi": [], "lasso": [], "rf": []}
    assert payload["consensus_topk"] == []


@pytest.mark.parametrize("threshold", ["nan", "inf", "-inf"])
def test_parse_args_rejects_nonfinite_redundancy_threshold(threshold):
    """The report threshold must never carry a JSON non-finite constant."""
    with pytest.raises(SystemExit) as exc_info:
        _parse_args(
            [
                "--parquet",
                "input.parquet",
                "--out",
                "report.json",
                "--redundancy-threshold",
                threshold,
            ]
        )

    assert exc_info.value.code == ARGPARSE_ERROR_CODE


def test_corr_main_filters_nonfinite_rows_and_writes_strict_json(tmp_path, monkeypatch, capsys):
    """Infinity is incomplete input, not a publishable correlation value."""
    parquet = tmp_path / "nonfinite.parquet"
    parquet.touch()
    out = tmp_path / "report.json"
    frame = pd.DataFrame(
        {
            "feat_a": [1.0, 2.0, np.inf, 4.0, -np.inf, 6.0],
            "feat_b": [2.0, 4.0, 6.0, 8.0, 10.0, 12.0],
            "vmaf": [60.0, 61.0, 62.0, np.inf, 64.0, 65.0],
        }
    )
    real_import = builtins.__import__

    def import_without_sklearn(name, *args, **kwargs):
        if name == "sklearn" or name.startswith("sklearn."):
            raise ImportError("sklearn deliberately unavailable")
        return real_import(name, *args, **kwargs)

    monkeypatch.setattr(pd, "read_parquet", lambda _path: frame)
    monkeypatch.setattr(builtins, "__import__", import_without_sklearn)

    assert corr_main(["--parquet", str(parquet), "--out", str(out)]) == 0
    assert "[corr] dropped non-finite rows: 3; clean rows=3" in capsys.readouterr().out
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
    assert payload["n_rows_clean"] == 3
    assert payload["feature_cols"] == ["feat_a", "feat_b"]
    assert payload["importances"] == {"mi": {}, "lasso": {}, "rf": {}}
    assert all(np.isfinite(value) for row in payload["pearson"].values() for value in row.values())


@pytest.mark.parametrize("value", [np.nan, np.inf, -np.inf])
def test_strict_report_refuses_nonfinite_payload(tmp_path, value):
    """The atomic writer must not publish Python-only JSON constants."""
    out = tmp_path / "report.json"

    with pytest.raises(ValueError, match="report contains a non-JSON value"):
        _write_strict_report(out, {"nested": {"value": value}})

    assert not out.exists()


def test_corr_main_rejects_table_without_usable_numeric_features(tmp_path, monkeypatch):
    """An all-unavailable feature schema must fail before NumPy or sklearn."""
    parquet = tmp_path / "syn_without_usable_features.parquet"
    pd.DataFrame(
        {
            "source": ["clipA", "clipB"],
            "codec": ["x264", "x265"],
            "feat_unavailable": [np.nan, np.nan],
            "vmaf": [80.0, 81.0],
        }
    ).to_parquet(parquet)
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(tmp_path / "x")],
    )

    with pytest.raises(ValueError, match="no usable numeric feature columns remain"):
        corr_main()


def test_corr_main_rejects_table_without_complete_rows(tmp_path, monkeypatch):
    parquet = tmp_path / "syn_without_complete_rows.parquet"
    pd.DataFrame(
        {
            "feat_a": [1.0, 2.0, np.nan, np.nan],
            "feat_b": [np.nan, np.nan, 2.0, 3.0],
            "vmaf": [80.0, 81.0, 82.0, 83.0],
        }
    ).to_parquet(parquet)
    monkeypatch.setattr(
        sys,
        "argv",
        ["feature_correlation.py", "--parquet", str(parquet), "--out", str(tmp_path / "x")],
    )

    with pytest.raises(ValueError, match="no complete rows remain"):
        corr_main()


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
    payload = json.loads(out.read_text(), parse_constant=_reject_nonfinite_json)
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
