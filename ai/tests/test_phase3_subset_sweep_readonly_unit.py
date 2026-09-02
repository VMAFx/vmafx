# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for the read-only-view fix in
:mod:`ai.scripts.phase3_subset_sweep`.

Pandas' ``DataFrame.to_numpy`` can return a read-only view of the
underlying block — particularly when the requested dtype matches the
column's storage dtype and no copy is needed. Production
``--standardize`` runs hit ``ValueError: output array is read-only`` at
the first ``-=`` inside ``_standardize_inplace`` because the LOSO
call-site fed the function exactly such a view.

The fix has two parts and both are pinned here:

1. ``_standardize_inplace`` now refuses read-only inputs up-front with a
   clear ``ValueError`` (rather than no-op'ing on a copy and silently
   dropping the standardisation step).
2. ``_loso_sweep`` forces a writeable copy via ``to_numpy(copy=True)`` so
   the production path always meets the contract.

Lives in a standalone file (not the broader ``_unit.py`` suite) to avoid
merge-conflicting with the in-flight PR #458 that adds the unit-test
file.
"""

from __future__ import annotations

import importlib.util
from pathlib import Path

import numpy as np
import pandas as pd
import pytest

_REPO_ROOT = Path(__file__).resolve().parents[2]
_SCRIPT = _REPO_ROOT / "ai" / "scripts" / "phase3_subset_sweep.py"


def _load_module():
    spec = importlib.util.spec_from_file_location("p3ss_readonly_under_test", _SCRIPT)
    assert spec is not None and spec.loader is not None
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


P3 = _load_module()


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


def test_standardize_inplace_raises_on_read_only_train() -> None:
    """A read-only x_train must raise a clear ``ValueError``, not the
    cryptic ``output array is read-only`` from the first ``-=``."""
    x_train = np.ones((10, 3), dtype=np.float64)
    x_val = np.ones((2, 3), dtype=np.float64)
    x_train.setflags(write=False)
    with pytest.raises(ValueError, match="writeable"):
        P3._standardize_inplace(x_train, x_val)


def test_standardize_inplace_raises_on_read_only_val() -> None:
    """A read-only x_val must also raise — otherwise the train side
    would be standardised but the val side would silently fail mid-loop.
    """
    x_train = np.ones((10, 3), dtype=np.float64)
    x_val = np.ones((2, 3), dtype=np.float64)
    x_val.setflags(write=False)
    with pytest.raises(ValueError, match="writeable"):
        P3._standardize_inplace(x_train, x_val)


def test_standardize_inplace_succeeds_on_writeable_inputs() -> None:
    """Sanity: writeable arrays still standardise to zero mean / unit
    std on the train fold (pre-existing behaviour preserved)."""
    rng = np.random.default_rng(7)
    x_train = rng.normal(loc=10.0, scale=3.0, size=(50, 4)).astype(np.float64)
    x_val = rng.normal(loc=10.0, scale=3.0, size=(10, 4)).astype(np.float64)
    P3._standardize_inplace(x_train, x_val)
    np.testing.assert_allclose(x_train.mean(axis=0), 0.0, atol=1e-10)
    np.testing.assert_allclose(x_train.std(axis=0), 1.0, atol=1e-10)


def test_loso_sweep_standardize_survives_readonly_pandas_view(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """End-to-end regression: the call site must materialise writeable
    arrays via ``to_numpy(copy=True)`` so ``--standardize`` works on
    parquet-backed DataFrames whose blocks pandas marks read-only.

    Wraps ``to_numpy`` so it returns a read-only view whenever called
    without ``copy=True``. If the production code regresses to
    ``to_numpy(dtype=…)`` without ``copy=True``, this test reproduces the
    exact ``ValueError: output array is read-only`` that bit production.
    """
    df = _toy_df()
    original_df_to_numpy = pd.DataFrame.to_numpy
    original_series_to_numpy = pd.Series.to_numpy

    def readonly_df_to_numpy(self, *args, **kwargs):  # type: ignore[no-untyped-def]
        arr = original_df_to_numpy(self, *args, **kwargs)
        if not kwargs.get("copy", False):
            arr = arr.view()
            arr.setflags(write=False)
        return arr

    def readonly_series_to_numpy(self, *args, **kwargs):  # type: ignore[no-untyped-def]
        arr = original_series_to_numpy(self, *args, **kwargs)
        if not kwargs.get("copy", False):
            arr = arr.view()
            arr.setflags(write=False)
        return arr

    monkeypatch.setattr(pd.DataFrame, "to_numpy", readonly_df_to_numpy)
    monkeypatch.setattr(pd.Series, "to_numpy", readonly_series_to_numpy)
    monkeypatch.setattr(
        P3,
        "_train_one_fold",
        lambda *a, **kw: {"plcc": 0.5, "srocc": 0.5, "rmse": 1.0},
    )
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
