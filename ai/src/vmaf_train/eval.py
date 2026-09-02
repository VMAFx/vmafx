# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Correlation + error metrics for trained FR/NR regressors.

Reports PLCC (Pearson), SROCC (Spearman), and RMSE. Accepts either:
  * a torch checkpoint (evaluated via lightning), or
  * an ONNX file (evaluated via onnxruntime CPU EP).
"""

from __future__ import annotations

import logging
import warnings
from dataclasses import dataclass
from pathlib import Path
from typing import Any, cast

import numpy as np

_log = logging.getLogger(__name__)

# Minimum variance below which an input array is considered degenerate
# (constant-valued). pearsonr/spearmanr return NaN for zero-variance inputs
# because the correlation coefficient is undefined. We return 0.0 instead so
# that gate logic using >= comparisons (e.g. bisect_model_quality._gate) sees
# "worst possible correlation" rather than a NaN that silently fails every
# comparison. See ADR-0963.
_VARIANCE_EPSILON = 1e-12


@dataclass
class EvalReport:
    plcc: float
    srocc: float
    rmse: float
    n: int

    def pretty(self) -> str:
        return (
            f"n={self.n:>6}  PLCC={self.plcc:+.4f}  SROCC={self.srocc:+.4f}  RMSE={self.rmse:.4f}"
        )


def correlations(pred: np.ndarray, target: np.ndarray) -> EvalReport:
    """Compute PLCC, SROCC, and RMSE between *pred* and *target*.

    Raises
    ------
    ValueError
        If either array is empty (length 0) or the shapes do not match.
        Empty inputs would yield NaN from scipy and from numpy, which
        silently corrupts ``bisect_model_quality._gate`` comparisons
        (``NaN >= threshold`` is always False). See ADR-0963.

    Notes
    -----
    When either array has zero variance (constant-valued), pearsonr and
    spearmanr are mathematically undefined and return NaN. In that case we
    emit a warning and return ``plcc=0.0, srocc=0.0`` (worst-case sentinel).
    This is preferable to NaN because gate logic uses ``>=`` comparisons and
    NaN propagation would falsely mark every model as failing. See ADR-0963.
    """
    if pred.shape != target.shape or pred.ndim != 1:
        raise ValueError("pred and target must be 1-D arrays of matching shape")
    if pred.shape[0] == 0:
        raise ValueError(
            "correlations() received empty inputs (length 0); "
            "this indicates a data-pipeline bug upstream. See ADR-0963."
        )

    from scipy.stats import pearsonr, spearmanr  # local import

    pred_var = float(np.var(pred))
    target_var = float(np.var(target))
    if pred_var < _VARIANCE_EPSILON or target_var < _VARIANCE_EPSILON:
        # Correlation is undefined for constant arrays. Return 0.0 (not NaN)
        # so that >= gate comparisons in bisect_model_quality work correctly.
        warnings.warn(
            f"correlations(): input has near-zero variance "
            f"(pred_var={pred_var:.2e}, target_var={target_var:.2e}); "
            "returning plcc=0.0, srocc=0.0. This typically means the model "
            "or training run is degenerate. See ADR-0963.",
            RuntimeWarning,
            stacklevel=2,
        )
        rmse = float(np.sqrt(((pred - target) ** 2).mean()))
        return EvalReport(plcc=0.0, srocc=0.0, rmse=rmse, n=pred.shape[0])

    # scipy returns *RResult dataclasses with ``.statistic`` / ``.pvalue``
    # at runtime but the bundled pyright stubs don't expose those attrs;
    # cast through Any so strict typecheck accepts the documented API.
    plcc = float(cast(Any, pearsonr(pred, target)).statistic)
    srocc = float(cast(Any, spearmanr(pred, target)).statistic)
    rmse = float(np.sqrt(((pred - target) ** 2).mean()))
    return EvalReport(plcc=plcc, srocc=srocc, rmse=rmse, n=pred.shape[0])


def evaluate_onnx(
    onnx_path: Path,
    features: np.ndarray,
    targets: np.ndarray,
    input_name: str = "input",
) -> EvalReport:
    import onnxruntime as ort

    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    out = sess.run(None, {input_name: features.astype(np.float32)})[0]
    pred = np.asarray(out).reshape(-1)
    return correlations(pred, targets.astype(np.float32))
