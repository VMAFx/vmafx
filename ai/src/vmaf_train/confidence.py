# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Heteroscedastic-regression utilities for tiny-AI MOS predictors.

When a model emits (score, logvar) instead of a single scalar it can
tell callers *how uncertain* the prediction is — a C1 model whose MOS
is 65 ± 3 is very different from 65 ± 15, even though the point
estimate is identical. This module holds the small, dependency-free
math that converts between logvar, σ, and confidence intervals so
inference-time consumers don't re-derive it.

The loss used in training is Gaussian NLL with a floor on σ to prevent
the model from gaming the objective by predicting huge variances on
hard samples:

    NLL = 0.5 * (logvar + (y - μ)² / exp(logvar))
"""

from __future__ import annotations

import math
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from torch import Tensor

MIN_LOGVAR = -6.0  # σ ≥ exp(-3) ≈ 0.05 MOS units, keeps NLL from diverging


def gaussian_nll(pred: "Tensor", target: "Tensor", logvar: "Tensor") -> "Tensor":
    """Per-sample Gaussian NLL with a soft floor on logvar.

    The constant 0.5 * log(2π) is omitted: it is independent of the
    network output and does not affect gradients.
    """
    import torch

    logvar = torch.clamp(logvar, min=MIN_LOGVAR)
    return 0.5 * (logvar + (target - pred) ** 2 / torch.exp(logvar))


def sigma_from_logvar(logvar: float) -> float:
    """σ = exp(0.5 · logvar)."""
    logvar = max(logvar, MIN_LOGVAR)
    return math.exp(0.5 * logvar)


def confidence_interval(score: float, logvar: float, z: float = 1.96) -> tuple[float, float]:
    """Return (low, high) bounds at @p z standard deviations.

    z=1.96 → 95% CI under the Gaussian assumption. Clamp score to the
    [0, 100] MOS range at the call site if you need a valid MOS.
    """
    sigma = sigma_from_logvar(logvar)
    half = z * sigma
    return score - half, score + half
