#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""LOSO eval harness for vmaf_tiny_v3 (mlp_medium) on the Netflix parquet.

Mirrors the methodology used to validate v2 (PLCC 0.9978 ± 0.0021) but
operates directly on the per-frame full-feature parquet rather than on
pre-exported per-fold ONNX checkpoints. For each of the 9 Netflix
``source`` values, trains a fresh mlp_medium on the union of the other
8 sources (with corpus-wide StandardScaler fit on those 8), then
evaluates PLCC / SROCC / RMSE on the held-out source.

Companion to the train_vmaf_tiny_v3 ship-gate. Writes
``runs/vmaf_tiny_v3_loso_metrics.json`` with per-fold + aggregate
statistics so the v3 → v2 PLCC delta can be cited directly in the ADR
+ research digest.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

from ai.scripts.train_vmaf_tiny_v3 import CANONICAL_6, _train

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root


def _metrics(pred: np.ndarray, y: np.ndarray) -> dict[str, float]:
    p = pred.astype(np.float64)
    t = y.astype(np.float64)
    plcc = float(np.corrcoef(p, t)[0, 1])
    rmse = float(np.sqrt(np.mean((p - t) ** 2)))
    rank_p = np.argsort(np.argsort(p))
    rank_t = np.argsort(np.argsort(t))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"n": len(y), "plcc": plcc, "srocc": srocc, "rmse": rmse}


def _eval_fold(
    model, mean: np.ndarray, std: np.ndarray, x_val: np.ndarray, y_val: np.ndarray
) -> dict[str, float]:
    import torch

    x_std = (x_val - mean) / std
    with torch.no_grad():
        pred = model(torch.from_numpy(x_std.astype(np.float32))).squeeze(-1).numpy()
    return _metrics(pred, y_val)


def _parse_args() -> argparse.Namespace:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--parquet",
        type=Path,
        required=True,
        help="Netflix per-frame parquet with 'source' column (9 unique values).",
    )
    ap.add_argument(
        "--out-json",
        type=Path,
        required=True,
        help="Output JSON with per-fold + aggregate metrics.",
    )
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    return ap.parse_args()


def _run_fold(df, held_out: str, args: argparse.Namespace) -> dict[str, float]:
    train_mask = df["source"] != held_out
    val_mask = df["source"] == held_out
    x_tr = df.loc[train_mask, list(CANONICAL_6)].to_numpy(dtype=np.float64)
    y_tr = df.loc[train_mask, "vmaf"].to_numpy(dtype=np.float64)
    x_va = df.loc[val_mask, list(CANONICAL_6)].to_numpy(dtype=np.float64)
    y_va = df.loc[val_mask, "vmaf"].to_numpy(dtype=np.float64)
    mean = x_tr.mean(axis=0)
    std = np.where(x_tr.std(axis=0, ddof=0) < 1e-8, 1.0, x_tr.std(axis=0, ddof=0))
    print(
        f"[loso-v3] fold={held_out}  train_rows={len(x_tr)}  val_rows={len(x_va)}",
        flush=True,
    )
    started = time.monotonic()
    model = _train(
        (x_tr - mean) / std,
        y_tr,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        seed=args.seed,
    )
    metrics = _eval_fold(model, mean, std, x_va, y_va)
    print(
        f"[loso-v3]   {held_out:14s} n={metrics['n']:4d} "
        f"PLCC={metrics['plcc']:.4f} SROCC={metrics['srocc']:.4f} "
        f"RMSE={metrics['rmse']:.3f} ({time.monotonic() - started:.1f}s)",
        flush=True,
    )
    return metrics


def _aggregate(folds: dict[str, dict[str, float]]) -> dict[str, float]:
    return {
        f"{stat}_{metric}": (
            float(getattr(np, stat)([row[metric] for row in folds.values()], ddof=1))
            if stat == "std"
            else float(np.mean([row[metric] for row in folds.values()]))
        )
        for stat in ("mean", "std")
        for metric in ("plcc", "srocc", "rmse")
    }


def _write_report(
    args: argparse.Namespace, sources: list[str], folds: dict, aggregate: dict
) -> None:
    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report = {
        "arch": "mlp_medium",
        "model": "vmaf_tiny_v3",
        "parquet": str(args.parquet),
        "n_folds": len(sources),
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "seed": args.seed,
        "per_fold": folds,
        "aggregate": aggregate,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=sys.argv[1:],
            args=args,
            inputs={"parquet": args.parquet},
            outputs={"report_target": str(args.out_json)},
        ),
    }
    write_manifest_json(args.out_json, report)


def main() -> int:
    args = _parse_args()

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    if "source" not in df.columns:
        print("error: parquet missing 'source' column", file=sys.stderr)
        return 2
    sources = sorted(df["source"].unique().tolist())
    print(f"[loso-v3] sources={sources} total_rows={len(df)}", flush=True)

    t_start = time.monotonic()
    fold_metrics = {held_out: _run_fold(df, held_out, args) for held_out in sources}
    aggregate = _aggregate(fold_metrics)
    print(
        f"[loso-v3] === aggregate over {len(fold_metrics)} folds ===\n"
        f"[loso-v3]  mean PLCC={aggregate['mean_plcc']:.4f} ± {aggregate['std_plcc']:.4f}\n"
        f"[loso-v3]  mean SROCC={aggregate['mean_srocc']:.4f} ± {aggregate['std_srocc']:.4f}\n"
        f"[loso-v3]  mean RMSE={aggregate['mean_rmse']:.3f} ± {aggregate['std_rmse']:.3f}\n"
        f"[loso-v3] total wall {time.monotonic() - t_start:.1f}s",
        flush=True,
    )

    _write_report(args, sources, fold_metrics, aggregate)
    print(f"[loso-v3] wrote {args.out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
