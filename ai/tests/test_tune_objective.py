# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for NaN-propagation guard in :func:`vmaf_train.tune._read_best_metric`.

Round-25 audit C.2: when every epoch produces NaN for the objective metric
(training diverged from epoch 0), the sweep objective must return
``float("inf")`` so Optuna's best-trial tracking is not corrupted.
See ADR-0963.
"""

from __future__ import annotations

import math
import warnings

import pandas as pd
import pytest

from vmaf_train.tune import _read_best_metric


class TestAllNanMetricsReturnsInf:
    """C.2a — all-NaN metric column must yield float("inf"), not NaN."""

    def test_all_nan_returns_inf(self) -> None:
        df = pd.DataFrame({"val/mse": [float("nan"), float("nan"), float("nan")]})
        result = _read_best_metric(df, "val/mse")
        assert math.isinf(result) and result > 0, "expected +inf for all-NaN column"

    def test_all_nan_emits_warning(self, caplog) -> None:
        import logging

        df = pd.DataFrame({"val/mse": [float("nan")]})
        with caplog.at_level(logging.WARNING, logger="vmaf_train.tune"):
            result = _read_best_metric(df, "val/mse")
        assert any(
            "all-NaN metrics" in msg for msg in caplog.messages
        ), "expected a WARNING log containing 'all-NaN metrics'"
        assert math.isinf(result) and result > 0

    def test_empty_column_returns_inf(self, caplog) -> None:
        """A completely empty column (no rows) also has no valid minimum."""
        import logging

        df = pd.DataFrame({"val/mse": pd.Series([], dtype=float)})
        with caplog.at_level(logging.WARNING, logger="vmaf_train.tune"):
            result = _read_best_metric(df, "val/mse")
        assert any(
            "all-NaN metrics" in msg for msg in caplog.messages
        ), "expected a WARNING log containing 'all-NaN metrics'"
        assert math.isinf(result) and result > 0

    def test_nan_result_is_not_passed_to_optuna(self) -> None:
        """Regression: the old ``float(df["val/mse"].dropna().min())`` returned
        NaN when all values were NaN. The guard must prevent that."""
        df = pd.DataFrame({"val/mse": [float("nan"), float("nan")]})
        result = _read_best_metric(df, "val/mse")
        assert not math.isnan(result), "NaN must not be returned as an Optuna objective"


class TestNormalMetricsReturnsMinValue:
    """C.2b — normal (non-NaN) metrics must return the actual minimum."""

    def test_returns_minimum_of_valid_values(self) -> None:
        df = pd.DataFrame({"val/mse": [0.8, 0.5, 0.3, 0.6]})
        result = _read_best_metric(df, "val/mse")
        assert result == pytest.approx(0.3)

    def test_skips_leading_nan_then_returns_min(self) -> None:
        """Early epochs often produce NaN before convergence — only finite
        values should be considered for the minimum."""
        df = pd.DataFrame({"val/mse": [float("nan"), float("nan"), 0.4, 0.2, 0.3]})
        # Should not warn because valid values exist
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            result = _read_best_metric(df, "val/mse")
        assert result == pytest.approx(0.2)

    def test_single_valid_value_returned(self) -> None:
        df = pd.DataFrame({"val/mse": [float("nan"), 0.7]})
        result = _read_best_metric(df, "val/mse")
        assert result == pytest.approx(0.7)

    def test_no_warning_for_normal_inputs(self) -> None:
        df = pd.DataFrame({"val/mse": [1.0, 0.8, 0.6]})
        with warnings.catch_warnings():
            warnings.simplefilter("error")
            result = _read_best_metric(df, "val/mse")
        assert result == pytest.approx(0.6)
