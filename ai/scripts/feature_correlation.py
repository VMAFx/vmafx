#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Research-0026 Phase 2 — feature correlation, mutual-information,
and importance ranking.

Reads a parquet of (frame, feature_columns..., vmaf) rows and emits:

  1. Pairwise Pearson correlation matrix (features only).
  2. Mutual-information from each feature to ``vmaf`` target.
  3. LASSO + random-forest feature importance (where sklearn available).
  4. Top-K consensus ranking across the three methods.

Output goes to a JSON report + a text summary printed to stdout.
"""

from __future__ import annotations

import argparse
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any

import numpy as np
from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


@dataclass(frozen=True)
class _AnalysisInput:
    x: np.ndarray
    y: np.ndarray
    feature_cols: list[str]
    skipped_non_numeric: list[str]
    skipped_all_nan: list[str]
    skipped_constant: list[str]
    n_rows_clean: int


def _pearson_matrix(x: np.ndarray, names: list[str]) -> dict:
    n = len(names)
    out = {names[i]: {names[j]: 0.0 for j in range(n)} for i in range(n)}
    corr = np.atleast_2d(np.corrcoef(x, rowvar=False))
    for i in range(n):
        for j in range(n):
            out[names[i]][names[j]] = float(corr[i, j])
    return out


def _redundant_pairs(x: np.ndarray, names: list[str], threshold: float) -> list[dict]:
    """Pairs with |Pearson r| ≥ threshold — redundant signal."""
    corr = np.atleast_2d(np.corrcoef(x, rowvar=False))
    pairs: list[dict] = []
    n = len(names)
    for i in range(n):
        for j in range(i + 1, n):
            r = float(corr[i, j])
            if abs(r) >= threshold:
                pairs.append({"a": names[i], "b": names[j], "r": r})
    return sorted(pairs, key=lambda p: -abs(p["r"]))


def _mutual_information_to_target(
    x: np.ndarray,
    y: np.ndarray,
    names: list[str],
) -> dict[str, float]:
    """Mutual information between each feature and ``y``."""
    try:
        from sklearn.feature_selection import mutual_info_regression
    except ImportError:
        print("  [skip] sklearn missing; mutual info skipped", file=sys.stderr)
        return {n: float("nan") for n in names}
    mi = mutual_info_regression(x, y, random_state=0)
    return {names[i]: float(mi[i]) for i in range(len(names))}


def _lasso_importance(x: np.ndarray, y: np.ndarray, names: list[str]) -> dict[str, float]:
    try:
        from sklearn.linear_model import LassoCV
        from sklearn.preprocessing import StandardScaler
    except ImportError:
        print("  [skip] sklearn missing; LASSO skipped", file=sys.stderr)
        return {n: float("nan") for n in names}
    scaler = StandardScaler()
    xz = scaler.fit_transform(x)
    model = LassoCV(cv=5, random_state=0, n_jobs=-1, max_iter=10000)
    model.fit(xz, y)
    return {names[i]: float(abs(model.coef_[i])) for i in range(len(names))}


def _random_forest_importance(x: np.ndarray, y: np.ndarray, names: list[str]) -> dict[str, float]:
    try:
        from sklearn.ensemble import RandomForestRegressor
    except ImportError:
        print("  [skip] sklearn missing; RF importance skipped", file=sys.stderr)
        return {n: float("nan") for n in names}
    rf = RandomForestRegressor(n_estimators=100, random_state=0, n_jobs=-1)
    rf.fit(x, y)
    return {names[i]: float(rf.feature_importances_[i]) for i in range(len(names))}


def _top_k_consensus(importances: dict[str, dict[str, float]], k: int) -> list[str]:
    """Features ranked top-K by EVERY method in `importances`."""
    sets: list[set[str]] = []
    for _method, scores in importances.items():
        finite = {n: v for n, v in scores.items() if not np.isnan(v)}
        if not finite:
            continue
        ranked = sorted(finite, key=lambda n: -finite[n])
        sets.append(set(ranked[:k]))
    if not sets:
        return []
    consensus = set.intersection(*sets)
    return sorted(consensus)


def _select_feature_columns(
    df: Any, candidate_cols: list[str]
) -> tuple[list[str], list[str], list[str], list[str]]:
    """Partition candidates into usable and explicitly skipped feature columns."""
    numeric_cols = list(df[candidate_cols].select_dtypes(include="number").columns)
    skipped_non_numeric = sorted(set(candidate_cols) - set(numeric_cols))
    skipped_all_nan = sorted(column for column in numeric_cols if not df[column].notna().any())
    all_nan = set(skipped_all_nan)
    populated = [column for column in numeric_cols if column not in all_nan]
    skipped_constant = sorted(column for column in populated if df[column].dropna().nunique() <= 1)
    constant = set(skipped_constant)
    usable = [column for column in populated if column not in constant]
    return usable, skipped_non_numeric, skipped_all_nan, skipped_constant


def _parse_args(raw_argv: list[str]) -> argparse.Namespace:
    ap = make_argument_parser(prog="feature_correlation.py")
    ap.add_argument("--parquet", type=Path, required=True)
    ap.add_argument(
        "--target",
        type=str,
        default="vmaf",
        help="Column name to use as the regression target.",
    )
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--redundancy-threshold",
        type=float,
        default=0.95,
        help="|Pearson r| above which pairs are flagged as redundant.",
    )
    ap.add_argument("--top-k", type=int, default=8)
    return ap.parse_args(raw_argv)


def _prepare_analysis_input(parquet: Path, target: str) -> _AnalysisInput:
    import pandas as pd

    df = pd.read_parquet(parquet)
    if target not in df.columns:
        raise ValueError(f"target column is missing: {target}")
    drop_cols = {"source", "dis_basename", "frame_index", "key", target}
    candidate_cols = [column for column in df.columns if column not in drop_cols]
    feat_cols, skipped_non_numeric, skipped_all_nan, skipped_constant = _select_feature_columns(
        df, candidate_cols
    )
    print(f"[corr] parquet={parquet} rows={len(df)} features={len(feat_cols)} target={target}")
    if skipped_non_numeric:
        print(f"[corr] skipped non-numeric columns: {skipped_non_numeric}")
    if skipped_all_nan:
        print(f"[corr] skipped all-NaN numeric columns: {skipped_all_nan}")
    if skipped_constant:
        print(f"[corr] skipped constant numeric columns: {skipped_constant}")
    if not feat_cols:
        raise ValueError("no usable numeric feature columns remain after filtering")

    df_clean = df.dropna(subset=[*feat_cols, target])
    print(f"[corr] dropped NaN rows: {len(df) - len(df_clean)}; clean rows={len(df_clean)}")
    if df_clean.empty:
        raise ValueError("no complete rows remain after dropping feature/target NaN values")
    return _AnalysisInput(
        x=df_clean[feat_cols].to_numpy(dtype=np.float64),
        y=df_clean[target].to_numpy(dtype=np.float64),
        feature_cols=feat_cols,
        skipped_non_numeric=skipped_non_numeric,
        skipped_all_nan=skipped_all_nan,
        skipped_constant=skipped_constant,
        n_rows_clean=len(df_clean),
    )


def _analyze(data: _AnalysisInput, threshold: float, top_k: int) -> dict[str, Any]:
    print("[corr] Pearson matrix...")
    pearson = _pearson_matrix(data.x, data.feature_cols)
    redundant = _redundant_pairs(data.x, data.feature_cols, threshold)
    print(f"[corr] redundant pairs (|r|>={threshold}): {len(redundant)}")
    for pair in redundant[:5]:
        print(f"        {pair['a']:<22} ↔ {pair['b']:<22} r={pair['r']:+.4f}")

    print("[corr] mutual information vs target...")
    mi = _mutual_information_to_target(data.x, data.y, data.feature_cols)
    print("[corr] LASSO importance...")
    lasso = _lasso_importance(data.x, data.y, data.feature_cols)
    print("[corr] random forest importance...")
    rf = _random_forest_importance(data.x, data.y, data.feature_cols)
    importances = {"mi": mi, "lasso": lasso, "rf": rf}
    consensus = _top_k_consensus(importances, top_k)
    print(f"[corr] top-{top_k} consensus ({len(consensus)}): {consensus}")

    per_method_topk: dict[str, list[tuple[str, float]]] = {}
    for method, scores in importances.items():
        finite = {name: value for name, value in scores.items() if not np.isnan(value)}
        per_method_topk[method] = sorted(finite.items(), key=lambda item: -item[1])[:top_k]
    return {
        "pearson": pearson,
        "redundant_pairs": redundant,
        "importances": importances,
        "per_method_topk": per_method_topk,
        "consensus_topk": consensus,
    }


def _build_report(
    args: argparse.Namespace,
    raw_argv: list[str],
    data: _AnalysisInput,
    analysis: dict[str, Any],
) -> dict[str, Any]:
    return {
        "parquet": str(args.parquet),
        "target": args.target,
        "n_rows_clean": data.n_rows_clean,
        "feature_cols": data.feature_cols,
        "skipped_non_numeric_columns": data.skipped_non_numeric,
        "skipped_all_nan_columns": data.skipped_all_nan,
        "skipped_constant_columns": data.skipped_constant,
        "pearson": analysis["pearson"],
        "redundant_pairs": analysis["redundant_pairs"],
        "redundancy_threshold": args.redundancy_threshold,
        "importances": analysis["importances"],
        "top_k": args.top_k,
        "per_method_topk": analysis["per_method_topk"],
        "consensus_topk": analysis["consensus_topk"],
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=vars(args),
            inputs={"parquet": args.parquet},
            outputs={"json_report": args.out},
        ),
    }


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    data = _prepare_analysis_input(args.parquet, args.target)
    analysis = _analyze(data, args.redundancy_threshold, args.top_k)
    write_manifest_json(args.out, _build_report(args, raw_argv, data, analysis))
    print(f"[corr] wrote {args.out}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
