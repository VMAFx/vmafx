#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Multi-seed leave-one-source-out eval for ``vmaf_tiny_v3`` + ``vmaf_tiny_v4``.

Mirrors the methodology of ``eval_loso_vmaf_tiny_v3.py`` (PR #294) and
``eval_loso_vmaf_tiny_v4.py`` (PR #299) but sweeps a configurable list
of random seeds and aggregates mean ± std across seeds. The same driver
covers two corpora because both parquets carry a ``source`` column whose
unique values define the held-out groups:

* Netflix per-frame parquet — 9 source clips (BigBuckBunny, BirdsInCage,
  CrowdRun, ElFuente1, ElFuente2, FoxBird, OldTownCross, Seeking, Tennis).
* KoNViD per-frame parquet (``runs/full_features_konvid_with_folds.parquet``)
  — 5 pre-assigned folds (``fold0``..``fold4``).

Architectures vendored inline (no upstream-of-master imports):

* ``mlp_medium`` — v3, 6→32→16→1, ~769 params.
* ``mlp_large``  — v4, 6→64→32→16→1, ~3 073 params.

Training recipe is identical to v3/v4 single-seed runs: corpus-wide
StandardScaler fit on the train fold, Adam @ lr=1e-3, MSE loss, 90
epochs, batch_size 256.

Usage::

    python ai/scripts/eval_multiseed_v3_v4.py \\
        --arch mlp_medium \\
        --parquet runs/full_features_netflix.parquet \\
        --out-json runs/vmaf_tiny_v3_loso_5seed.json \\
        --seeds 0 1 2 3 4

This script is **eval-only** — it does not export ONNX. v3/v4 ONNX
artefacts in ``model/tiny/`` remain the seed=0 productions; the
multi-seed sweep produces confidence intervals, not new shipped models.
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

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _build_mlp_medium(in_dim: int):  # type: ignore[no-untyped-def]
    """v3 architecture — 6 → 32 → 16 → 1 (vendored from train_vmaf_tiny_v3.py)."""
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 32),
        nn.ReLU(),
        nn.Linear(32, 16),
        nn.ReLU(),
        nn.Linear(16, 1),
    )


def _build_mlp_large(in_dim: int):  # type: ignore[no-untyped-def]
    """v4 architecture — 6 → 64 → 32 → 16 → 1 (vendored from train_vmaf_tiny_v4.py)."""
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 64),
        nn.ReLU(),
        nn.Linear(64, 32),
        nn.ReLU(),
        nn.Linear(32, 16),
        nn.ReLU(),
        nn.Linear(16, 1),
    )


_BUILDERS = {
    "mlp_medium": _build_mlp_medium,
    "mlp_large": _build_mlp_large,
}


def _train(
    x: np.ndarray,
    y: np.ndarray,
    *,
    arch: str,
    epochs: int,
    batch_size: int,
    lr: float,
    seed: int,
):  # type: ignore[no-untyped-def]
    """Train @p arch on the standardised feature matrix.

    Identical loop to v3/v4 — only the model factory differs.
    """
    import torch
    from torch import nn

    torch.manual_seed(seed)
    in_dim = x.shape[1]
    model = _BUILDERS[arch](in_dim)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = nn.MSELoss()

    x_t = torch.from_numpy(x.astype(np.float32))
    y_t = torch.from_numpy(y.astype(np.float32)).unsqueeze(-1)
    n = x_t.shape[0]
    rng = np.random.default_rng(seed)

    for _ep in range(epochs):
        model.train()
        perm = rng.permutation(n)
        for start in range(0, n, batch_size):
            idx = torch.from_numpy(perm[start : start + batch_size])
            xb = x_t.index_select(0, idx)
            yb = y_t.index_select(0, idx)
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()
    return model.eval()


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


def _run_one_seed(
    df,
    *,
    arch: str,
    sources: list[str],
    seed: int,
    epochs: int,
    batch_size: int,
    lr: float,
) -> dict[str, dict[str, float]]:
    fold_metrics: dict[str, dict[str, float]] = {}
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

        t0 = time.monotonic()
        model = _train(
            x_tr_std,
            y_tr,
            arch=arch,
            epochs=epochs,
            batch_size=batch_size,
            lr=lr,
            seed=seed,
        )
        m = _eval_fold(model, mean, std, x_va, y_va)
        fold_metrics[held_out] = m
        print(
            f"[ms-{arch}] seed={seed} fold={held_out:14s} n={m['n']:5d} "
            f"PLCC={m['plcc']:.4f} SROCC={m['srocc']:.4f} RMSE={m['rmse']:.3f} "
            f"({time.monotonic() - t0:.1f}s)",
            flush=True,
        )
    return fold_metrics


def _aggregate(per_seed: dict[int, dict[str, dict[str, float]]]) -> dict:
    seeds = sorted(per_seed.keys())
    sources = sorted(per_seed[seeds[0]].keys())

    # Per-seed across-fold means.
    per_seed_agg: dict[int, dict[str, float]] = {}
    for s in seeds:
        plccs = [per_seed[s][src]["plcc"] for src in sources]
        sroccs = [per_seed[s][src]["srocc"] for src in sources]
        rmses = [per_seed[s][src]["rmse"] for src in sources]
        per_seed_agg[s] = {
            "mean_plcc": float(np.mean(plccs)),
            "mean_srocc": float(np.mean(sroccs)),
            "mean_rmse": float(np.mean(rmses)),
        }

    # Per-fold across-seed means.
    per_fold_agg: dict[str, dict[str, float]] = {}
    for src in sources:
        plccs = [per_seed[s][src]["plcc"] for s in seeds]
        sroccs = [per_seed[s][src]["srocc"] for s in seeds]
        rmses = [per_seed[s][src]["rmse"] for s in seeds]
        per_fold_agg[src] = {
            "mean_plcc": float(np.mean(plccs)),
            "std_plcc": float(np.std(plccs, ddof=1)) if len(plccs) > 1 else 0.0,
            "mean_srocc": float(np.mean(sroccs)),
            "std_srocc": float(np.std(sroccs, ddof=1)) if len(sroccs) > 1 else 0.0,
            "mean_rmse": float(np.mean(rmses)),
            "std_rmse": float(np.std(rmses, ddof=1)) if len(rmses) > 1 else 0.0,
        }

    # Across-seed aggregate of the per-seed-mean PLCC (the headline number).
    seed_means_plcc = [per_seed_agg[s]["mean_plcc"] for s in seeds]
    seed_means_srocc = [per_seed_agg[s]["mean_srocc"] for s in seeds]
    seed_means_rmse = [per_seed_agg[s]["mean_rmse"] for s in seeds]
    overall = {
        "mean_plcc": float(np.mean(seed_means_plcc)),
        "std_plcc": float(np.std(seed_means_plcc, ddof=1)) if len(seeds) > 1 else 0.0,
        "mean_srocc": float(np.mean(seed_means_srocc)),
        "std_srocc": float(np.std(seed_means_srocc, ddof=1)) if len(seeds) > 1 else 0.0,
        "mean_rmse": float(np.mean(seed_means_rmse)),
        "std_rmse": float(np.std(seed_means_rmse, ddof=1)) if len(seeds) > 1 else 0.0,
    }
    return {
        "per_seed_aggregate": per_seed_agg,
        "per_fold_aggregate": per_fold_agg,
        "overall": overall,
    }


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument(
        "--arch",
        choices=sorted(_BUILDERS.keys()),
        required=True,
        help="Architecture: mlp_medium (v3) or mlp_large (v4).",
    )
    ap.add_argument(
        "--parquet",
        type=Path,
        required=True,
        help="Per-frame parquet with 'source' + 'vmaf' columns.",
    )
    ap.add_argument(
        "--out-json",
        type=Path,
        required=True,
        help="Output JSON with per-seed per-fold metrics + aggregates.",
    )
    ap.add_argument("--seeds", type=int, nargs="+", default=[0, 1, 2, 3, 4])
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument(
        "--label",
        type=str,
        default="",
        help="Free-form label for the output JSON (e.g. 'netflix-loso', 'konvid-5fold').",
    )
    args = ap.parse_args()

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    if "source" not in df.columns:
        print("error: parquet missing 'source' column", file=sys.stderr)
        return 2
    missing = [c for c in CANONICAL_6 if c not in df.columns]
    if missing:
        print(f"error: parquet missing feature columns: {missing}", file=sys.stderr)
        return 2
    if "vmaf" not in df.columns:
        print("error: parquet missing 'vmaf' target column", file=sys.stderr)
        return 2

    sources = sorted(df["source"].unique().tolist())
    print(
        f"[ms-{args.arch}] parquet={args.parquet} rows={len(df)} "
        f"sources={sources} seeds={args.seeds}",
        flush=True,
    )

    per_seed: dict[int, dict[str, dict[str, float]]] = {}
    t_start = time.monotonic()
    for seed in args.seeds:
        print(f"[ms-{args.arch}] === seed {seed} ===", flush=True)
        per_seed[seed] = _run_one_seed(
            df,
            arch=args.arch,
            sources=sources,
            seed=seed,
            epochs=args.epochs,
            batch_size=args.batch_size,
            lr=args.lr,
        )

    agg = _aggregate(per_seed)
    print(
        f"[ms-{args.arch}] === overall across {len(args.seeds)} seeds ===\n"
        f"[ms-{args.arch}]  mean PLCC = {agg['overall']['mean_plcc']:.4f} "
        f"± {agg['overall']['std_plcc']:.4f}\n"
        f"[ms-{args.arch}]  mean SROCC= {agg['overall']['mean_srocc']:.4f} "
        f"± {agg['overall']['std_srocc']:.4f}\n"
        f"[ms-{args.arch}]  mean RMSE = {agg['overall']['mean_rmse']:.3f} "
        f"± {agg['overall']['std_rmse']:.3f}\n"
        f"[ms-{args.arch}] total wall {time.monotonic() - t_start:.1f}s",
        flush=True,
    )

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report = {
        "arch": args.arch,
        "label": args.label,
        "parquet": str(args.parquet),
        "n_folds": len(sources),
        "sources": sources,
        "seeds": args.seeds,
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "per_seed_per_fold": {str(s): per_seed[s] for s in args.seeds},
        **agg,
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
    print(f"[ms-{args.arch}] wrote {args.out_json}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
