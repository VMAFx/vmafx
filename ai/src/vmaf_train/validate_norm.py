# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Validate that a shipped sidecar's normalization matches the data it's fed.

A common silent-correctness bug: a C1 model was trained with feature
normalization computed over dataset A, but it's being deployed against
dataset B with a different distribution. The model still outputs a
score, but PLCC collapses and nobody notices until a QA engineer gets
suspicious numbers.

This module loads a parquet feature cache, looks up the mean/std from
the paired sidecar, and reports per-feature deviation. A >3σ drift on
any feature column is flagged.
"""

from __future__ import annotations

import json
from dataclasses import dataclass, field
from pathlib import Path

import numpy as np

from .features import FEATURE_COLUMNS

OUTLIER_SIGMA = 3.0
OUTLIER_FRACTION_WARN = 0.05  # 5% of samples > 3σ → warn


@dataclass
class FeatureDrift:
    name: str
    declared_mean: float
    declared_std: float
    observed_mean: float
    observed_std: float
    mean_shift_sigma: float  # |observed - declared| / declared_std
    outlier_fraction: float  # samples >3σ from declared mean


@dataclass
class NormReport:
    sidecar: Path
    n_samples: int
    drifts: list[FeatureDrift] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)

    @property
    def ok(self) -> bool:
        return not self.warnings

    def to_dict(self) -> dict:
        return {
            "sidecar": str(self.sidecar),
            "n_samples": self.n_samples,
            "drifts": [d.__dict__ for d in self.drifts],
            "warnings": list(self.warnings),
            "ok": self.ok,
        }


def _load_sidecar(onnx_or_sidecar: Path) -> tuple[Path, dict]:
    sidecar = (
        onnx_or_sidecar
        if onnx_or_sidecar.suffix == ".json"
        else onnx_or_sidecar.with_suffix(".json")
    )
    if not sidecar.is_file():
        raise FileNotFoundError(f"sidecar not found: {sidecar}")
    return sidecar, json.loads(sidecar.read_text())


def _load_features(path: Path, columns: list[str]) -> tuple[np.ndarray, list[str]]:
    import pandas as pd

    df = pd.read_parquet(path)
    cols = [c for c in columns if c in df.columns]
    if not cols:
        raise ValueError(
            f"{path} has none of the expected feature columns; " f"available: {list(df.columns)}"
        )
    return df[cols].to_numpy(dtype=np.float64), cols


def validate_norm(
    model: Path,
    features: Path,
    columns: list[str] | None = None,
) -> NormReport:
    sidecar_path, meta = _load_sidecar(model)
    norm = meta.get("normalization") or {}
    mean = norm.get("mean")
    std = norm.get("std")
    cols = list(columns or FEATURE_COLUMNS)

    if not mean or not std:
        report = NormReport(sidecar=sidecar_path, n_samples=0)
        report.warnings.append("sidecar has no normalization block — cannot validate drift")
        return report

    mean_arr = np.asarray(mean, dtype=np.float64)
    std_arr = np.asarray(std, dtype=np.float64)
    if len(mean_arr) != len(cols):
        raise ValueError(
            f"sidecar normalization has {len(mean_arr)} entries but expected "
            f"{len(cols)} (columns: {cols})"
        )

    x, resolved_cols = _load_features(features, cols)
    report = NormReport(sidecar=sidecar_path, n_samples=int(x.shape[0]))

    for i, name in enumerate(resolved_cols):
        col = x[:, i]
        declared_mean = float(mean_arr[i])
        declared_std = float(std_arr[i])
        obs_mean = float(col.mean())
        obs_std = float(col.std(ddof=0))
        shift = abs(obs_mean - declared_mean) / max(declared_std, 1e-9)
        outliers = float(np.mean(np.abs(col - declared_mean) > OUTLIER_SIGMA * declared_std))
        drift = FeatureDrift(
            name=name,
            declared_mean=declared_mean,
            declared_std=declared_std,
            observed_mean=obs_mean,
            observed_std=obs_std,
            mean_shift_sigma=shift,
            outlier_fraction=outliers,
        )
        report.drifts.append(drift)
        if shift > 1.0:
            report.warnings.append(f"{name}: declared mean drift {shift:.2f}σ from observed data")
        if outliers > OUTLIER_FRACTION_WARN:
            report.warnings.append(
                f"{name}: {outliers * 100:.1f}% of samples >3σ from declared mean "
                f"(threshold {OUTLIER_FRACTION_WARN * 100:.0f}%)"
            )

    return report


def render_table(report: NormReport) -> str:
    if not report.drifts:
        status = "no normalization block" if not report.warnings else report.warnings[0]
        return f"({report.sidecar.name}) {status}"

    lines = [
        f"sidecar: {report.sidecar.name}   samples: {report.n_samples}",
        (
            f"{'feature':<14} {'decl. μ':>10} {'decl. σ':>10} {'obs μ':>10} "
            f"{'obs σ':>10} {'Δμ/σ':>8} {'>3σ':>7}"
        ),
        "-" * 74,
    ]
    for d in report.drifts:
        lines.append(
            f"{d.name:<14} {d.declared_mean:>10.4g} {d.declared_std:>10.4g} "
            f"{d.observed_mean:>10.4g} {d.observed_std:>10.4g} "
            f"{d.mean_shift_sigma:>7.2f}  {d.outlier_fraction * 100:>5.1f}%"
        )
    if report.warnings:
        lines.append("")
        for w in report.warnings:
            lines.append(f"  WARN: {w}")
    return "\n".join(lines)
