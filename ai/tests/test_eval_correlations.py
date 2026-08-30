# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for NaN-propagation guards in :func:`vmaf_train.eval.correlations`.

Round-25 audit C.1: guard empty / degenerate inputs in ``correlations()``.
See ADR-0963.
"""

from __future__ import annotations

import warnings

import numpy as np
import pytest

from vmaf_train.eval import EvalReport, correlations


class TestEmptyInputsRaiseValueError:
    """C.1a — empty arrays must raise, not silently return NaN."""

    def test_empty_pred_raises(self) -> None:
        with pytest.raises(ValueError, match="empty inputs"):
            correlations(np.zeros(0), np.zeros(0))

    def test_empty_pred_nonempty_target_caught_by_shape_check(self) -> None:
        # Shape mismatch fires before the emptiness guard — still a ValueError.
        with pytest.raises(ValueError):
            correlations(np.zeros(0), np.zeros(3))


class TestConstantInputsReturnsZeroCorrelationWithWarning:
    """C.1b — constant-valued arrays must produce 0.0 correlation (not NaN)
    and emit a RuntimeWarning so the caller can diagnose the degenerate run.

    Gate logic in ``bisect_model_quality._gate`` does ``report.plcc >= value``.
    NaN >= threshold is always False, which would silently make every model
    "fail" the bisect and report "first model already bad". Returning 0.0
    makes the semantics explicit: degenerate run = worst-case correlation.
    """

    def test_constant_pred_returns_zero_plcc_srocc(self) -> None:
        pred = np.ones(16, dtype=np.float32)
        target = np.linspace(0.0, 1.0, 16).astype(np.float32)
        with pytest.warns(RuntimeWarning, match="near-zero variance"):
            report = correlations(pred, target)
        assert report.plcc == 0.0
        assert report.srocc == 0.0
        assert report.n == 16

    def test_constant_target_returns_zero_plcc_srocc(self) -> None:
        pred = np.linspace(0.0, 1.0, 16).astype(np.float32)
        target = np.full(16, 42.0, dtype=np.float32)
        with pytest.warns(RuntimeWarning, match="near-zero variance"):
            report = correlations(pred, target)
        assert report.plcc == 0.0
        assert report.srocc == 0.0

    def test_constant_inputs_rmse_still_computed(self) -> None:
        """RMSE is meaningful even when correlation is degenerate."""
        pred = np.full(8, 5.0, dtype=np.float64)
        target = np.full(8, 3.0, dtype=np.float64)
        with pytest.warns(RuntimeWarning):
            report = correlations(pred, target)
        assert report.rmse == pytest.approx(2.0)

    def test_zero_sentinel_does_not_fail_gate_comparison(self) -> None:
        """0.0 >= 0.9 is False (expected), not the NaN-false that was silent."""
        pred = np.ones(8, dtype=np.float32)
        target = np.linspace(0.0, 1.0, 8).astype(np.float32)
        with pytest.warns(RuntimeWarning):
            report = correlations(pred, target)
        # The gate check itself must not raise or return a NaN
        result = report.plcc >= 0.9
        assert result is False
        # And the value must be exactly 0.0 (not NaN)
        assert not np.isnan(report.plcc)
        assert not np.isnan(report.srocc)


class TestNormalInputsUnchanged:
    """C.1c — regression: normal inputs must produce the same results as before."""

    def test_perfect_correlation(self) -> None:
        rng = np.random.default_rng(42)
        target = rng.uniform(0.0, 100.0, 64).astype(np.float64)
        report = correlations(target, target)
        assert report.plcc == pytest.approx(1.0)
        assert report.srocc == pytest.approx(1.0)
        assert report.rmse == pytest.approx(0.0, abs=1e-6)
        assert report.n == 64

    def test_anti_correlation(self) -> None:
        target = np.linspace(0.0, 1.0, 32, dtype=np.float64)
        pred = -target
        report = correlations(pred, target)
        assert report.plcc == pytest.approx(-1.0)
        assert report.srocc == pytest.approx(-1.0)

    def test_no_warning_for_normal_inputs(self) -> None:
        rng = np.random.default_rng(0)
        pred = rng.uniform(0.0, 1.0, 50)
        target = rng.uniform(0.0, 1.0, 50)
        with warnings.catch_warnings():
            warnings.simplefilter("error", RuntimeWarning)
            report = correlations(pred, target)
        assert isinstance(report, EvalReport)

    def test_shape_mismatch_still_raises(self) -> None:
        with pytest.raises(ValueError, match="matching shape"):
            correlations(np.zeros(3), np.zeros(4))

    def test_2d_input_raises(self) -> None:
        with pytest.raises(ValueError):
            correlations(np.zeros((3, 2)), np.zeros((3, 2)))  # type: ignore[arg-type]
