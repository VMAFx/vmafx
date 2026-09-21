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
from pathlib import Path
from typing import TYPE_CHECKING, Any

import numpy as np

if TYPE_CHECKING:
    from ai.scripts._script_bootstrap import bootstrap_ai_script
else:
    try:
        from ai.scripts._script_bootstrap import bootstrap_ai_script
    except ModuleNotFoundError:
        from _script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__, include_repo_root=True)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402


def _pearson_matrix(x: np.ndarray, names: list[str]) -> dict[str, dict[str, float]]:
    n = len(names)
    out = {names[i]: {names[j]: 0.0 for j in range(n)} for i in range(n)}
    corr = np.corrcoef(x, rowvar=False)
    for i in range(n):
        for j in range(n):
            out[names[i]][names[j]] = float(corr[i, j])
    return out


def _redundant_pairs(x: np.ndarray, names: list[str], threshold: float) -> list[dict[str, Any]]:
    """Pairs with |Pearson r| ≥ threshold — redundant signal."""
    corr = np.corrcoef(x, rowvar=False)
    pairs: list[dict[str, Any]] = []
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


def _load_feature_arrays(args: argparse.Namespace) -> tuple[list[str], np.ndarray, np.ndarray, int]:
    import pandas as pd

    frame = pd.read_parquet(args.parquet)
    drop_cols = {"source", "dis_basename", "frame_index", "key", args.target}
    feature_columns = [column for column in frame.columns if column not in drop_cols]
    print(
        f"[corr] parquet={args.parquet} rows={len(frame)} features={len(feature_columns)} "
        f"target={args.target}"
    )
    clean = frame.dropna(subset=[*feature_columns, args.target])
    print(f"[corr] dropped NaN rows: {len(frame) - len(clean)}; clean rows={len(clean)}")
    x = clean[feature_columns].to_numpy(dtype=np.float64)
    y = clean[args.target].to_numpy(dtype=np.float64)
    return feature_columns, x, y, len(clean)


def _calculate_importances(
    x: np.ndarray,
    y: np.ndarray,
    feature_columns: list[str],
) -> dict[str, dict[str, float]]:
    print("[corr] mutual information vs target...")
    mutual_information = _mutual_information_to_target(x, y, feature_columns)
    print("[corr] LASSO importance...")
    lasso = _lasso_importance(x, y, feature_columns)
    print("[corr] random forest importance...")
    random_forest = _random_forest_importance(x, y, feature_columns)
    return {"mi": mutual_information, "lasso": lasso, "rf": random_forest}


def _rank_importances(
    importances: dict[str, dict[str, float]], top_k: int
) -> dict[str, list[tuple[str, float]]]:
    rankings: dict[str, list[tuple[str, float]]] = {}
    for method, scores in importances.items():
        finite = {name: value for name, value in scores.items() if not np.isnan(value)}
        rankings[method] = sorted(finite.items(), key=lambda item: -item[1])[:top_k]
    return rankings


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    feature_columns, x, y, clean_rows = _load_feature_arrays(args)

    print("[corr] Pearson matrix...")
    pearson = _pearson_matrix(x, feature_columns)
    redundant = _redundant_pairs(x, feature_columns, args.redundancy_threshold)
    print(f"[corr] redundant pairs (|r|>={args.redundancy_threshold}): {len(redundant)}")
    for pair in redundant[:5]:
        print(f"        {pair['a']:<22} ↔ {pair['b']:<22} r={pair['r']:+.4f}")

    importances = _calculate_importances(x, y, feature_columns)
    consensus = _top_k_consensus(importances, args.top_k)
    print(f"[corr] top-{args.top_k} consensus ({len(consensus)}): {consensus}")
    report = {
        "parquet": str(args.parquet),
        "target": args.target,
        "n_rows_clean": clean_rows,
        "feature_cols": feature_columns,
        "pearson": pearson,
        "redundant_pairs": redundant,
        "redundancy_threshold": args.redundancy_threshold,
        "importances": importances,
        "top_k": args.top_k,
        "per_method_topk": _rank_importances(importances, args.top_k),
        "consensus_topk": consensus,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=vars(args),
            inputs={"parquet": args.parquet},
            outputs={"json_report": args.out},
        ),
    }
    write_manifest_json(args.out, report)
    print(f"[corr] wrote {args.out}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
