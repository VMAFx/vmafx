# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Heteroscedastic-regression math used by FRRegressor / NRMetric variance mode."""

from __future__ import annotations

import math

import pytest

from vmaf_train.confidence import MIN_LOGVAR, confidence_interval, sigma_from_logvar


def test_sigma_from_logvar_round_trip() -> None:
    # logvar = 2·log(σ); sigma_from_logvar(2·log(σ)) == σ for σ above the floor.
    for sigma in (0.1, 1.0, 5.0, 20.0):
        logvar = 2 * math.log(sigma)
        assert sigma_from_logvar(logvar) == pytest.approx(sigma, rel=1e-6)


def test_sigma_clamps_at_floor() -> None:
    # Anything below the floor should clamp to exp(MIN_LOGVAR / 2).
    sigma_floor = math.exp(MIN_LOGVAR / 2)
    assert sigma_from_logvar(-1000.0) == pytest.approx(sigma_floor)
    assert sigma_from_logvar(MIN_LOGVAR) == pytest.approx(sigma_floor)


def test_confidence_interval_symmetric_around_score() -> None:
    low, high = confidence_interval(score=75.0, logvar=0.0, z=1.96)
    # logvar=0 → σ=1 → 95% CI = 75 ± 1.96
    assert low == pytest.approx(75.0 - 1.96, rel=1e-9)
    assert high == pytest.approx(75.0 + 1.96, rel=1e-9)


def test_confidence_interval_widens_with_logvar() -> None:
    _, high_tight = confidence_interval(score=60.0, logvar=0.0)
    _, high_wide = confidence_interval(score=60.0, logvar=2.0)
    # logvar=2 → σ=e, so the wide CI should be ~e× the tight one.
    width_tight = high_tight - 60.0
    width_wide = high_wide - 60.0
    assert width_wide > width_tight * 2.5


def test_gaussian_nll_minimum_at_truth() -> None:
    """NLL of the Gaussian should be minimized when μ = y, for any logvar."""
    torch = pytest.importorskip("torch")
    from vmaf_train.confidence import gaussian_nll

    y = torch.tensor([50.0])
    logvar = torch.tensor([0.0])  # σ = 1
    # Loss at the truth should be lower than at any offset.
    loss_at_truth = gaussian_nll(y, y, logvar).item()
    loss_offset_1 = gaussian_nll(y + 1.0, y, logvar).item()
    loss_offset_5 = gaussian_nll(y + 5.0, y, logvar).item()
    assert loss_at_truth < loss_offset_1 < loss_offset_5


def test_gaussian_nll_respects_logvar_floor() -> None:
    """A crazy-negative logvar must be clamped to avoid division blow-up."""
    torch = pytest.importorskip("torch")
    from vmaf_train.confidence import gaussian_nll

    pred = torch.tensor([10.0])
    target = torch.tensor([11.0])
    crazy = torch.tensor([-100.0])
    loss = gaussian_nll(pred, target, crazy)
    # Finite, not NaN / inf.
    assert torch.isfinite(loss).all()
