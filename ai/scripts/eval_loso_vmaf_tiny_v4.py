#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""LOSO eval harness for vmaf_tiny_v4 (mlp_large) on the Netflix parquet.

Mirrors the methodology used to validate v2 (PLCC 0.9978 ± 0.0021) and
v3 (PLCC 0.9986 ± 0.0015) but operates directly on the per-frame
full-feature parquet rather than on pre-exported per-fold ONNX
checkpoints. For each of the 9 Netflix ``source`` values, trains a
fresh mlp_large on the union of the other 8 sources (with corpus-wide
StandardScaler fit on those 8), then evaluates PLCC / SROCC / RMSE on
the held-out source.

Companion to the train_vmaf_tiny_v4 ship-gate. Writes
``runs/vmaf_tiny_v4_loso_metrics.json`` with per-fold + aggregate
statistics so the v4 → v3 PLCC delta can be cited directly in the ADR
+ research digest.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path

import numpy as np

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT))

from ai.scripts.train_vmaf_tiny_v4 import CANONICAL_6, _train  # noqa: E402


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


def main() -> int:
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
    args = ap.parse_args()

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    if "source" not in df.columns:
        print("error: parquet missing 'source' column", file=sys.stderr)
        return 2
    sources = sorted(df["source"].unique().tolist())
    print(f"[loso-v4] sources={sources} total_rows={len(df)}", flush=True)

    fold_metrics: dict[str, dict[str, float]] = {}
    plccs, sroccs, rmses = [], [], []
    t_start = time.monotonic()
    for held_out in sources:
        train_mask = df["source"] != held_out
        val_mask = df["source"] == held_out
        x_tr = df.loc[train_mask, list(CANONICAL_6)].to_numpy(dtype=np.float64)
        y_tr = df.loc[train_mask, "vmaf"].to_numpy(dtype=np.float64)
        x_va = df.loc[val_mask, list(CANONICAL_6)].to_numpy(dtype=np.float64)
        y_va = df.loc[val_mask, "vmaf"].to_numpy(dtype=np.float64)

        mean = x_tr.mean(axis=0)
        std = x_tr.std(axis=0, ddof=0)
        std = np.where(std < 1e-8, 1.0, std)
        x_tr_std = (x_tr - mean) / std

        print(
            f"[loso-v4] fold={held_out}  train_rows={len(x_tr)}  val_rows={len(x_va)}",
            flush=True,
        )
        t0 = time.monotonic()
        model = _train(
            x_tr_std,
            y_tr,
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.lr,
            seed=args.seed,
        )
        m = _eval_fold(model, mean, std, x_va, y_va)
        fold_metrics[held_out] = m
        plccs.append(m["plcc"])
        sroccs.append(m["srocc"])
        rmses.append(m["rmse"])
        print(
            f"[loso-v4]   {held_out:14s} n={m['n']:4d} "
            f"PLCC={m['plcc']:.4f} SROCC={m['srocc']:.4f} RMSE={m['rmse']:.3f} "
            f"({time.monotonic() - t0:.1f}s)",
            flush=True,
        )

    aggregate = {
        "mean_plcc": float(np.mean(plccs)),
        "mean_srocc": float(np.mean(sroccs)),
        "mean_rmse": float(np.mean(rmses)),
        "std_plcc": float(np.std(plccs, ddof=1)),
        "std_srocc": float(np.std(sroccs, ddof=1)),
        "std_rmse": float(np.std(rmses, ddof=1)),
    }
    print(
        f"[loso-v4] === aggregate over {len(plccs)} folds ===\n"
        f"[loso-v4]  mean PLCC={aggregate['mean_plcc']:.4f} ± {aggregate['std_plcc']:.4f}\n"
        f"[loso-v4]  mean SROCC={aggregate['mean_srocc']:.4f} ± {aggregate['std_srocc']:.4f}\n"
        f"[loso-v4]  mean RMSE={aggregate['mean_rmse']:.3f} ± {aggregate['std_rmse']:.3f}\n"
        f"[loso-v4] total wall {time.monotonic() - t_start:.1f}s",
        flush=True,
    )

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report = {
        "arch": "mlp_large",
        "model": "vmaf_tiny_v4",
        "parquet": str(args.parquet),
        "n_folds": len(sources),
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "seed": args.seed,
        "per_fold": fold_metrics,
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
    print(f"[loso-v4] wrote {args.out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
