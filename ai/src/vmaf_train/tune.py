# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Optional Optuna hyperparameter sweep — imported lazily to keep core deps small."""

from __future__ import annotations

import logging
from collections.abc import Callable
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    import optuna
    import pandas as pd

    from .train import TrainConfig

_log = logging.getLogger(__name__)


def _read_best_metric(df: "pd.DataFrame", col: str) -> float:  # type: ignore[name-defined]
    """Return the minimum non-NaN value in *col*, or ``float("inf")`` if none exist.

    When every epoch produced NaN for *col* (training diverged from epoch 0),
    ``dropna().min()`` returns NaN. Passing NaN to Optuna as a Trial objective
    corrupts the study's best-trial tracking because Optuna stores it as a
    completed trial with an undefined comparison value. We return
    ``float("inf")`` (worst value for a minimisation study) and log a warning
    so operators can investigate the diverged run. See ADR-0963.

    Note: this function assumes the study direction is ``"minimize"``
    (the only direction used in ``sweep()``). A ``"maximize"`` study would
    need ``float("-inf")"`` — if that ever changes, update this helper and its
    callers accordingly.
    """
    valid = df[col].dropna()
    if valid.empty or np.isnan(valid.min()):
        _log.warning(
            "tune.objective: column %r contains no finite values "
            "(all-NaN metrics — training likely diverged). "
            "Returning float('inf') as the worst-case objective so Optuna "
            "does not corrupt the study's best-trial tracking. "
            "Inspect the trial output directory for convergence details. "
            "See ADR-0963.",
            col,
        )
        return float("inf")
    return float(valid.min())


def sweep(
    base_cfg: "TrainConfig",
    suggest: Callable[["optuna.Trial"], dict[str, object]],
    n_trials: int = 20,
    study_name: str = "vmaf-train-sweep",
    storage: str | None = None,
) -> "optuna.Study":
    import optuna  # local import — optional dep

    from .train import TrainConfig, train  # local import — heavy deps, keep deferred

    def objective(trial: "optuna.Trial") -> float:
        overrides = suggest(trial)
        cfg = TrainConfig(
            model=base_cfg.model,
            model_args={**base_cfg.model_args, **overrides},
            cache=base_cfg.cache,
            output=Path(base_cfg.output) / f"trial_{trial.number:03d}",
            epochs=base_cfg.epochs,
            batch_size=base_cfg.batch_size,
            val_frac=base_cfg.val_frac,
            test_frac=base_cfg.test_frac,
            seed=base_cfg.seed,
            precision=base_cfg.precision,
        )
        train(cfg)
        metrics_csv = cfg.output / "lightning_logs" / "version_0" / "metrics.csv"
        if not metrics_csv.exists():
            return float("inf")
        import pandas as pd

        df = pd.read_csv(metrics_csv)
        if "val/mse" in df.columns:
            return _read_best_metric(df, "val/mse")
        if "val/l1" in df.columns:
            return _read_best_metric(df, "val/l1")
        return float("inf")

    study = optuna.create_study(
        study_name=study_name,
        storage=storage,
        direction="minimize",
        load_if_exists=bool(storage),
    )
    study.optimize(objective, n_trials=n_trials)
    return study
