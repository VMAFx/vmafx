# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``aiutils.retrain_checks``: NaN never passes a data check or a gate."""

from __future__ import annotations

import importlib
from typing import Any, Callable, TypeVar, cast

import numpy as np
import pytest

from aiutils.retrain_checks import (
    DataCheckError,
    Thresholds,
    gate_verdict,
    metrics,
    require_finite_columns,
)

pd: Any = importlib.import_module("pandas")
F = TypeVar("F", bound=Callable[..., Any])


def _parametrize(*args: Any, **kwargs: Any) -> Callable[[F], F]:
    return cast(Callable[[F], F], pytest.mark.parametrize(*args, **kwargs))


def _frame() -> Any:
    return pd.DataFrame({"adm2": [0.9, 0.95, 1.0], "vmaf": [70.0, 80.0, 99.0]})


def test_finite_table_passes() -> None:
    require_finite_columns(_frame(), ["adm2", "vmaf"], what="t")


def test_missing_column_is_named() -> None:
    with pytest.raises(DataCheckError, match=r"missing column\(s\) \['motion2'\]"):
        require_finite_columns(_frame(), ["adm2", "motion2"], what="table t")


@_parametrize("bad", [float("nan"), float("inf"), float("-inf")])
def test_non_finite_value_is_named_with_column_and_row(bad: float) -> None:
    df = _frame()
    df.loc[1, "adm2"] = bad
    with pytest.raises(
        DataCheckError, match=r"column 'adm2' holds 1 non-finite value\(s\) of 3 \(first at row 1\)"
    ):
        require_finite_columns(df, ["adm2", "vmaf"], what="table t")


def test_metrics_of_a_perfect_fit() -> None:
    y = np.array([10.0, 20.0, 30.0, 40.0])
    m = metrics(y, y)
    assert m == {"plcc": pytest.approx(1.0), "srocc": pytest.approx(1.0), "rmse": 0.0}


def test_constant_prediction_has_no_correlation() -> None:
    m = metrics(np.full(4, 5.0), np.array([1.0, 2.0, 3.0, 4.0]))
    assert np.isnan(m["plcc"]) and np.isnan(m["srocc"])
    assert m["rmse"] > 0


def test_srocc_uses_average_ranks_for_ties() -> None:
    # With arbitrary tie-breaking the tied pair would rank 0,1 or 1,0 and the
    # result would depend on input order; average ranks make it 1.0 here.
    pred = np.array([1.0, 1.0, 2.0, 3.0])
    truth = np.array([5.0, 5.0, 6.0, 7.0])
    assert metrics(pred, truth)["srocc"] == pytest.approx(1.0)


def test_shape_mismatch_is_refused() -> None:
    with pytest.raises(DataCheckError, match="shape"):
        metrics(np.zeros(3), np.zeros(4))


def test_gate_passes_a_good_model() -> None:
    good = {"plcc": 0.99, "srocc": 0.98, "rmse": 2.0}
    assert gate_verdict(good, Thresholds(min_plcc=0.95, min_srocc=0.9, max_rmse=5.0)) == []


def test_gate_fails_each_bound_with_its_name() -> None:
    low = {"plcc": 0.5, "srocc": 0.5, "rmse": 9.0}
    reasons = gate_verdict(low, Thresholds(min_plcc=0.9, min_srocc=0.9, max_rmse=5.0))
    assert [r.split()[0] for r in reasons] == ["PLCC", "SROCC", "RMSE"]


def test_gate_fails_a_model_that_predicts_a_constant() -> None:
    # `plcc < gate` is False for NaN, so the naive comparison passes this model.
    m = metrics(np.full(5, 3.0), np.arange(5.0))
    naive_pass = not (m["plcc"] < 0.9)
    assert naive_pass is True
    reasons = gate_verdict(m, Thresholds(min_plcc=0.9))
    assert reasons and "not finite" in reasons[0]


def test_gate_fails_non_finite_metric_even_without_a_bound() -> None:
    reasons = gate_verdict(
        {"plcc": float("nan"), "srocc": 1.0, "rmse": 1.0}, Thresholds(max_rmse=5.0)
    )
    assert reasons
