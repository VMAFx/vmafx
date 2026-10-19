# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Data and gate checks shared by the retrain driver and the trainers (ADR-1898).

Two jobs, both "fail named, never pass on NaN":

* :func:`require_finite_columns` refuses a feature table whose named columns hold
  NaN or infinity, and names the column and the first offending row. Trainers call it
  before fitting: a NaN in a feature standardises to NaN, trains to NaN and
  exports a model whose every prediction is NaN, with exit status 0.
* :func:`metrics` and :func:`gate_verdict` compute PLCC, SROCC and RMSE and
  compare them with a threshold, where a NaN metric (a constant prediction has
  no correlation) is a failure. The comparison ``plcc < gate`` is false for NaN,
  so a gate written that way passes the model that predicts a constant.
"""

from __future__ import annotations

import math
from collections.abc import Mapping, Sequence
from dataclasses import dataclass
from typing import Any

import numpy as np


class DataCheckError(ValueError):
    """A feature table or a metric fails a retrain check; the message names it."""


def require_finite_columns(df: Any, columns: Sequence[str], *, what: str) -> None:
    """Raise :class:`DataCheckError` unless every ``columns`` entry exists and is finite."""
    missing = [c for c in columns if c not in df.columns]
    if missing:
        raise DataCheckError(f"{what}: missing column(s) {missing}")
    for column in columns:
        values = np.asarray(df[column].to_numpy(dtype=np.float64))
        bad = ~np.isfinite(values)
        if bad.any():
            first = int(np.flatnonzero(bad)[0])
            raise DataCheckError(
                f"{what}: column '{column}' holds {int(bad.sum())} non-finite value(s) "
                f"of {len(values)} (first at row {first})"
            )


def _rank(values: np.ndarray) -> np.ndarray:
    """Average ranks (ties share a rank), so SROCC of tied data is not arbitrary."""
    order = np.argsort(values, kind="mergesort")
    ranks = np.empty(len(values), dtype=np.float64)
    sorted_values = values[order]
    start = 0
    while start < len(values):
        stop = start
        while stop + 1 < len(values) and sorted_values[stop + 1] == sorted_values[start]:
            stop += 1
        ranks[order[start : stop + 1]] = (start + stop) / 2.0
        start = stop + 1
    return ranks


def _corr(a: np.ndarray, b: np.ndarray) -> float:
    if len(a) < 2 or float(np.std(a)) == 0.0 or float(np.std(b)) == 0.0:
        return float("nan")
    return float(np.corrcoef(a, b)[0, 1])


def metrics(pred: np.ndarray, truth: np.ndarray) -> dict[str, float]:
    """PLCC, SROCC and RMSE of ``pred`` against ``truth``; NaN where undefined."""
    p = np.asarray(pred, dtype=np.float64).reshape(-1)
    t = np.asarray(truth, dtype=np.float64).reshape(-1)
    if p.shape != t.shape:
        raise DataCheckError(f"prediction shape {p.shape} differs from truth shape {t.shape}")
    return {
        "plcc": _corr(p, t),
        "srocc": _corr(_rank(p), _rank(t)),
        "rmse": float(np.sqrt(np.mean((p - t) ** 2))) if len(p) else float("nan"),
    }


@dataclass(frozen=True)
class Thresholds:
    """A model's gate: minimum PLCC and SROCC, maximum RMSE (``None`` = recorded only)."""

    min_plcc: float | None = None
    min_srocc: float | None = None
    max_rmse: float | None = None


def _breaches(value: float, bound: float | None, name: str, *, floor: bool) -> list[str]:
    if bound is None:
        return []
    if not math.isfinite(value):
        return [f"{name} is {value} (not finite), gate {'>=' if floor else '<='} {bound}"]
    if floor and value < bound:
        return [f"{name} {value:.4f} < gate {bound}"]
    if not floor and value > bound:
        return [f"{name} {value:.4f} > gate {bound}"]
    return []


def gate_verdict(measured: Mapping[str, float], gate: Thresholds) -> list[str]:
    """Reasons ``measured`` fails ``gate``; empty means the model passes."""
    out: list[str] = [
        f"{name} is {measured[key]} (not finite)"
        for key, name in (("plcc", "PLCC"), ("srocc", "SROCC"), ("rmse", "RMSE"))
        if not math.isfinite(measured[key])
    ]
    if out:
        return out
    out += _breaches(measured["plcc"], gate.min_plcc, "PLCC", floor=True)
    out += _breaches(measured["srocc"], gate.min_srocc, "SROCC", floor=True)
    out += _breaches(measured["rmse"], gate.max_rmse, "RMSE", floor=False)
    return out
