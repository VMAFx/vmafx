# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for :mod:`ai.scripts.phase3_subset_sweep` helpers.

Targets the pure-numpy + LOSO pipeline functions:
``_standardize_inplace``, ``_summary``, ``_loso_sweep`` (with a tiny MLP
training stub), and the ``main`` argparse / IO surface. Avoids any real
torch backward passes by patching ``_train_one_fold`` to deterministic
fake metrics.
"""

from __future__ import annotations

import importlib.util
import json
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "phase3_subset_sweep.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("p3ss_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


P3 = _load_module()


# ---------------------------------------------------------------------------
# SUBSETS registry
# ---------------------------------------------------------------------------


def test_subsets_canonical6_matches_vmaf_v061_features() -> None:
    expected = {
        "adm2",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion2",
    }
    assert set(P3.SUBSETS["canonical6"]) == expected


def test_subsets_a_extends_canonical6_with_ssimulacra2() -> None:
    assert set(P3.SUBSETS["A"]) - set(P3.SUBSETS["canonical6"]) == {"ssimulacra2"}


def test_subsets_c_contains_all_extractors_under_test() -> None:
    # C is the "full-21 sanity ceiling" — must include canonical6.
    assert set(P3.SUBSETS["canonical6"]).issubset(set(P3.SUBSETS["C"]))


# ---------------------------------------------------------------------------
# _standardize_inplace
# ---------------------------------------------------------------------------


def test_standardize_inplace_centers_train() -> None:
    rng = np.random.default_rng(7)
    x_train = rng.normal(loc=10.0, scale=3.0, size=(50, 4)).astype(np.float64)
    x_val = rng.normal(loc=10.0, scale=3.0, size=(10, 4)).astype(np.float64)
    P3._standardize_inplace(x_train, x_val)
    np.testing.assert_allclose(x_train.mean(axis=0), 0.0, atol=1e-10)
    np.testing.assert_allclose(x_train.std(axis=0), 1.0, atol=1e-10)


def test_standardize_inplace_uses_train_stats_on_val() -> None:
    # Constant 7s in train → mean 7, std 0 → falls back to 1.0; val centred by 7.
    x_train = np.full((10, 3), 7.0)
    x_val = np.array([[10.0, 10.0, 10.0]], dtype=np.float64)
    P3._standardize_inplace(x_train, x_val)
    np.testing.assert_allclose(x_val[0], [3.0, 3.0, 3.0])


def test_standardize_inplace_floor_protects_constant_columns() -> None:
    x_train = np.zeros((4, 2), dtype=np.float64)
    x_val = np.zeros((1, 2), dtype=np.float64)
    P3._standardize_inplace(x_train, x_val)
    # No div-by-zero — output is all zeros (no NaN/inf).
    assert not np.isnan(x_train).any()
    assert not np.isnan(x_val).any()


# ---------------------------------------------------------------------------
# _summary
# ---------------------------------------------------------------------------


def test_summary_aggregates_per_fold_metrics() -> None:
    per_fold = {
        "src-a": {"plcc": 0.9, "srocc": 0.85, "rmse": 4.0},
        "src-b": {"plcc": 0.8, "srocc": 0.75, "rmse": 5.0},
        "src-c": {"plcc": 0.95, "srocc": 0.92, "rmse": 3.5},
    }
    s = P3._summary(per_fold)
    assert s["n_folds"] == 3
    assert s["mean_plcc"] == pytest.approx(0.8833333, rel=1e-5)
    assert s["mean_srocc"] == pytest.approx(0.84, abs=1e-3)
    assert s["mean_rmse"] == pytest.approx(4.1666667, rel=1e-5)
    assert s["std_plcc"] > 0


def test_summary_empty_returns_nan_means() -> None:
    # Empty per_fold dict -> mean is NaN (numpy convention).
    with pytest.warns(RuntimeWarning):
        s = P3._summary({})
    assert s["n_folds"] == 0
    assert np.isnan(s["mean_plcc"])


# ---------------------------------------------------------------------------
# _loso_sweep — patch _train_one_fold to deterministic outputs
# ---------------------------------------------------------------------------


def _toy_df() -> pd.DataFrame:
    rows = []
    for source in ("src-a", "src-b", "src-c"):
        for i in range(8):
            rows.append(
                {
                    "source": source,
                    "adm2": 0.5 + i * 0.01,
                    "vif_scale0": 0.6 + i * 0.01,
                    "vif_scale1": 0.65 + i * 0.01,
                    "vif_scale2": 0.7 + i * 0.01,
                    "vif_scale3": 0.75 + i * 0.01,
                    "motion2": 1.0 + i,
                    "vmaf": 80.0 + i,
                }
            )
    return pd.DataFrame(rows)


def test_loso_sweep_runs_one_fold_per_source(monkeypatch: pytest.MonkeyPatch) -> None:
    df = _toy_df()

    calls = []

    def fake_train(x_train, y_train, x_val, y_val, *, epochs, batch_size, lr, seed):
        calls.append({"n_train": len(x_train), "n_val": len(x_val), "seed": seed})
        return {"plcc": 0.9, "srocc": 0.88, "rmse": 3.0}

    monkeypatch.setattr(P3, "_train_one_fold", fake_train)
    per_fold = P3._loso_sweep(
        df,
        P3.SUBSETS["canonical6"],
        epochs=1,
        batch_size=4,
        lr=1e-3,
        seed=42,
    )
    assert set(per_fold.keys()) == {"src-a", "src-b", "src-c"}
    assert len(calls) == 3
    # Sanity: each fold's val=8 rows (one held-out source × 8), train=16.
    for c in calls:
        assert c["n_val"] == 8
        assert c["n_train"] == 16


def test_loso_sweep_standardize_path_invokes_standardiser(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    df = _toy_df()
    monkeypatch.setattr(
        P3,
        "_train_one_fold",
        lambda *a, **kw: {"plcc": 0.5, "srocc": 0.5, "rmse": 1.0},
    )
    # _standardize_inplace mutates arrays in-place — pandas may return
    # read-only views, which the production path then writes through
    # numpy ufuncs. Patch the standardiser to a no-op for this test and
    # verify it's invoked on every (train, val) pair.
    calls: list[tuple[int, int]] = []

    def fake_standardize(x_train, x_val):
        calls.append((x_train.shape[0], x_val.shape[0]))

    monkeypatch.setattr(P3, "_standardize_inplace", fake_standardize)
    per_fold = P3._loso_sweep(
        df,
        P3.SUBSETS["canonical6"],
        epochs=1,
        batch_size=4,
        lr=1e-3,
        seed=0,
        standardize=True,
    )
    assert len(per_fold) == 3
    assert len(calls) == 3  # one standardise per fold


# ---------------------------------------------------------------------------
# main entry point
# ---------------------------------------------------------------------------


def test_main_returns_2_on_unknown_subset(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    parquet = tmp_path / "f.parquet"
    df = _toy_df()
    df.to_parquet(parquet)
    monkeypatch.setattr(
        P3,
        "_train_one_fold",
        lambda *a, **kw: {"plcc": 0.5, "srocc": 0.5, "rmse": 1.0},
    )
    rc = P3.main(
        [
            "--parquet",
            str(parquet),
            "--out",
            str(tmp_path / "out.json"),
            "--subsets",
            "DOES_NOT_EXIST",
            "--epochs",
            "1",
        ]
    )
    assert rc == 2


def test_main_returns_2_when_subset_columns_missing(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    parquet = tmp_path / "f.parquet"
    df = _toy_df().drop(columns=["motion2"])
    df.to_parquet(parquet)
    monkeypatch.setattr(
        P3,
        "_train_one_fold",
        lambda *a, **kw: {"plcc": 0.5, "srocc": 0.5, "rmse": 1.0},
    )
    rc = P3.main(
        [
            "--parquet",
            str(parquet),
            "--out",
            str(tmp_path / "out.json"),
            "--subsets",
            "canonical6",
            "--epochs",
            "1",
        ]
    )
    assert rc == 2


def test_main_writes_report_with_canonical6_summary(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    parquet = tmp_path / "f.parquet"
    out = tmp_path / "report.json"
    df = _toy_df()
    df.to_parquet(parquet)
    monkeypatch.setattr(
        P3,
        "_train_one_fold",
        lambda *a, **kw: {"plcc": 0.9, "srocc": 0.88, "rmse": 3.0},
    )
    rc = P3.main(
        [
            "--parquet",
            str(parquet),
            "--out",
            str(out),
            "--subsets",
            "canonical6",
            "--epochs",
            "1",
        ]
    )
    assert rc == 0
    assert out.is_file()
    payload = json.loads(out.read_text())
    assert "canonical6" in payload
    summary = payload["canonical6"]["summary"]
    assert summary["mean_plcc"] == pytest.approx(0.9)
    assert summary["n_folds"] == 3


def test_main_supports_seeds_list(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    parquet = tmp_path / "f.parquet"
    out = tmp_path / "report.json"
    df = _toy_df()
    df.to_parquet(parquet)

    seed_observed = []

    def fake_train(x_train, y_train, x_val, y_val, *, epochs, batch_size, lr, seed):
        seed_observed.append(seed)
        return {"plcc": 0.9, "srocc": 0.88, "rmse": 3.0}

    monkeypatch.setattr(P3, "_train_one_fold", fake_train)
    rc = P3.main(
        [
            "--parquet",
            str(parquet),
            "--out",
            str(out),
            "--subsets",
            "canonical6",
            "--epochs",
            "1",
            "--seeds",
            "1,2",
        ]
    )
    assert rc == 0
    payload = json.loads(out.read_text())
    assert payload["canonical6"]["summary"]["n_seeds"] == 2
    # Each seed visits each fold once → 2 seeds × 3 folds.
    assert sorted(seed_observed) == [1, 1, 1, 2, 2, 2]
