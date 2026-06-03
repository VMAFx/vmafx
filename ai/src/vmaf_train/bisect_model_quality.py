# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Bisect a list of ONNX model checkpoints for the first quality regression.

A sibling of the code-level ``/bisect-regression`` skill. Where that one
walks a git history looking for a bad commit, this one walks an *ordered
list of model artifacts* (e.g. checkpoints from a training run, or the
sequence of models shipped over successive releases) and finds the first
one that falls below a PLCC / SROCC / RMSE gate on a held-out set.

The contract assumes monotonic quality on the list: the head is good,
the tail is bad. If both ends are good or both are bad we bail out
instead of silently binary-searching a nonsensical interval, so the
caller has to fix their list before proceeding.
"""

from __future__ import annotations

from dataclasses import asdict, dataclass, field
from pathlib import Path

import numpy as np

from .eval import EvalReport, evaluate_onnx


@dataclass
class BisectStep:
    index: int
    model: Path
    report: EvalReport
    passed: bool


@dataclass
class BisectResult:
    threshold_kind: str  # "min_plcc" | "min_srocc" | "max_rmse"
    threshold_value: float
    n_models: int
    first_bad_index: int | None = None
    first_bad_model: Path | None = None
    last_good_index: int | None = None
    last_good_model: Path | None = None
    steps: list[BisectStep] = field(default_factory=list)
    verdict: str = ""  # free-form human-readable summary

    def to_dict(self) -> dict:
        d = asdict(self)
        d["first_bad_model"] = str(self.first_bad_model) if self.first_bad_model else None
        d["last_good_model"] = str(self.last_good_model) if self.last_good_model else None
        for s in d["steps"]:
            s["model"] = str(s["model"])
        return d


def _gate(kind: str, value: float, report: EvalReport) -> bool:
    """True when @p report satisfies the gate (i.e. model is good)."""
    if kind == "min_plcc":
        return report.plcc >= value
    if kind == "min_srocc":
        return report.srocc >= value
    if kind == "max_rmse":
        return report.rmse <= value
    raise ValueError(f"unknown threshold_kind: {kind!r}")


def _resolve_threshold(
    min_plcc: float | None,
    min_srocc: float | None,
    max_rmse: float | None,
) -> tuple[str, float]:
    chosen = [
        (k, v)
        for k, v in [
            ("min_plcc", min_plcc),
            ("min_srocc", min_srocc),
            ("max_rmse", max_rmse),
        ]
        if v is not None
    ]
    if len(chosen) != 1:
        raise ValueError(
            "bisect_model_quality needs exactly one of min_plcc / min_srocc / max_rmse"
        )
    return chosen[0]


def bisect_model_quality(
    models: list[Path],
    features: np.ndarray,
    targets: np.ndarray,
    min_plcc: float | None = None,
    min_srocc: float | None = None,
    max_rmse: float | None = None,
    input_name: str = "input",
) -> BisectResult:
    """Binary-search @p models for the first one that fails the gate.

    Evaluates each candidate model via the shared ``eval_metrics`` path
    so the PLCC/SROCC/RMSE numbers match what ``vmaf-train eval`` would
    report on the same inputs.
    """
    if len(models) < 2:
        raise ValueError("bisect_model_quality needs at least 2 models")

    kind, threshold = _resolve_threshold(min_plcc, min_srocc, max_rmse)
    result = BisectResult(threshold_kind=kind, threshold_value=threshold, n_models=len(models))
    cache: dict[int, BisectStep] = {}

    def check(idx: int) -> BisectStep:
        if idx in cache:
            return cache[idx]
        model = models[idx]
        report = evaluate_onnx(model, features, targets, input_name=input_name)
        step = BisectStep(
            index=idx,
            model=model,
            report=report,
            passed=_gate(kind, threshold, report),
        )
        cache[idx] = step
        result.steps.append(step)
        return step

    first = check(0)
    last = check(len(models) - 1)
    if not first.passed:
        result.verdict = "first model (index 0) already fails gate; nothing to bisect"
        result.first_bad_index = 0
        result.first_bad_model = models[0]
        return result
    if last.passed:
        result.verdict = "last model still passes gate; no regression in this range"
        result.last_good_index = len(models) - 1
        result.last_good_model = models[-1]
        return result

    lo, hi = 0, len(models) - 1
    while hi - lo > 1:
        mid = (lo + hi) // 2
        step = check(mid)
        if step.passed:
            lo = mid
        else:
            hi = mid

    result.first_bad_index = hi
    result.first_bad_model = models[hi]
    result.last_good_index = lo
    result.last_good_model = models[lo]
    result.verdict = (
        f"first bad: index {hi} ({models[hi].name}) — " f"last good: index {lo} ({models[lo].name})"
    )
    # Keep the step log in visit order for auditability.
    result.steps.sort(key=lambda s: s.index)
    return result


def render_table(result: BisectResult) -> str:
    lines = [
        f"threshold: {result.threshold_kind} = {result.threshold_value:g}",
        f"models: {result.n_models}   visited: {len(result.steps)}",
        "-" * 72,
        f"{'idx':>4} {'status':>6} {'PLCC':>8} {'SROCC':>8} {'RMSE':>8}  model",
    ]
    for s in result.steps:
        status = "GOOD" if s.passed else "BAD"
        lines.append(
            f"{s.index:>4} {status:>6} "
            f"{s.report.plcc:>+8.4f} {s.report.srocc:>+8.4f} "
            f"{s.report.rmse:>8.4f}  {s.model.name}"
        )
    lines.append("")
    lines.append(result.verdict)
    return "\n".join(lines)
