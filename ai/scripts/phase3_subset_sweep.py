#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Research-0027 Phase-3 — LOSO MLP arch sweep on Top-K subsets.

Reads the full-feature parquet produced by
``ai/scripts/extract_full_features.py``, runs a 9-fold
leave-one-source-out training sweep per feature subset, and reports
mean ± std PLCC / SROCC / RMSE per subset.

Subsets per Research-0027 §"Recommended Phase-3 subsets":

* ``canonical6``    — vmaf_v0.6.1 baseline (DEFAULT_FEATURES).
* ``A``             — canonical6 ∪ {ssimulacra2}.
* ``B``             — consensus-7 (canonical core + adm_scale3
                       + ssimulacra2 + psnr_hvs + float_ssim, drop
                       redundant vif scales).
* ``C``             — full-21 sanity ceiling.

Stopping rule per Research-0027 §"Decision":

  If A's mean LOSO PLCC fails to beat canonical6 by ≥ 0.005, the
  hypothesis is dead; B and C still run for completeness but the
  default model recommendation stays canonical6.

Output: ``runs/phase3_subset_sweep.json`` with per-subset, per-fold
metrics + summary table. Stdout pretty-prints the comparison.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

SUBSETS: dict[str, tuple[str, ...]] = {
    "canonical6": (
        "adm2",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion2",
    ),
    "A": (
        "adm2",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion2",
        "ssimulacra2",
    ),
    "B": (
        "adm2",
        "adm_scale3",
        "vif_scale2",
        "motion2",
        "ssimulacra2",
        "psnr_hvs",
        "float_ssim",
    ),
    "C": (
        "adm2",
        "adm_scale0",
        "adm_scale1",
        "adm_scale2",
        "adm_scale3",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion",
        "motion2",
        "motion3",
        "psnr_y",
        "psnr_cb",
        "psnr_cr",
        "float_ssim",
        "float_ms_ssim",
        "cambi",
        "ciede2000",
        "psnr_hvs",
        "ssimulacra2",
    ),
}


def _build_mlp_small(in_dim: int):  # type: ignore[no-untyped-def]
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 16),
        nn.ReLU(),
        nn.Linear(16, 8),
        nn.ReLU(),
        nn.Linear(8, 1),
    )


def _train_one_fold(
    x_train: np.ndarray,
    y_train: np.ndarray,
    x_val: np.ndarray,
    y_val: np.ndarray,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    seed: int,
) -> dict[str, float]:
    import torch
    from torch import nn
    from torch.utils.data import DataLoader, TensorDataset

    torch.manual_seed(seed)
    in_dim = x_train.shape[1]
    model = _build_mlp_small(in_dim)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = nn.MSELoss()

    ds = TensorDataset(
        torch.from_numpy(x_train.astype(np.float32)),
        torch.from_numpy(y_train.astype(np.float32)).unsqueeze(-1),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)

    for _ in range(epochs):
        model.train()
        for xb, yb in loader:
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()

    model.eval()
    with torch.no_grad():
        preds = model(torch.from_numpy(x_val.astype(np.float32))).squeeze(-1).numpy()

    pred = preds.astype(np.float64)
    target = y_val.astype(np.float64)
    plcc = float(np.corrcoef(pred, target)[0, 1])
    rmse = float(np.sqrt(np.mean((pred - target) ** 2)))
    rank_p = np.argsort(np.argsort(pred))
    rank_t = np.argsort(np.argsort(target))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"plcc": plcc, "srocc": srocc, "rmse": rmse}


def _standardize_inplace(x_train: np.ndarray, x_val: np.ndarray) -> None:
    """Per-column standardisation: fit (mean, std) on train, apply to both.

    Uses the train fold's statistics only — never peek at the val fold —
    so the LOSO methodology stays honest. The constant 1e-8 floor on
    `std` avoids divide-by-zero on degenerate features (none in the
    current feature pool, but cheap insurance).

    Contract: both arrays MUST be writeable. Pandas' ``DataFrame.to_numpy``
    can hand back read-only views of the underlying block (especially when
    no dtype conversion is needed, e.g. an all-``float64`` parquet column);
    writing through a read-only view raises
    ``ValueError: output array is read-only`` at the first ``-=`` below.
    The ``_loso_sweep`` call site forces a copy via ``to_numpy(copy=True)``
    so it always meets the contract. This function fails fast with a clear
    message if a future caller forgets — silently no-op'ing on a copy would
    drop the standardisation and skew downstream metrics.
    """
    if not x_train.flags.writeable or not x_val.flags.writeable:
        raise ValueError(
            "_standardize_inplace requires writeable arrays; got "
            f"x_train.writeable={x_train.flags.writeable}, "
            f"x_val.writeable={x_val.flags.writeable}. "
            "Pass arrays produced with to_numpy(copy=True) or call .copy()."
        )
    mean = x_train.mean(axis=0)
    std = x_train.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_train -= mean
    x_train /= std
    x_val -= mean
    x_val /= std


def _loso_sweep(
    df,  # type: ignore[no-untyped-def]
    feature_cols: tuple[str, ...],
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    seed: int,
    standardize: bool = False,
) -> dict[str, dict[str, float]]:
    sources = sorted(df["source"].unique())
    per_fold: dict[str, dict[str, float]] = {}
    for held_out in sources:
        train_mask = df["source"] != held_out
        val_mask = df["source"] == held_out
        # copy=True: pandas may otherwise return a read-only view of the
        # parquet-backed block, which _standardize_inplace can't write
        # through. Cheap (one fold-sized allocation) vs. the
        # ValueError: output array is read-only failure mode.
        x_train = df.loc[train_mask, list(feature_cols)].to_numpy(dtype=np.float64, copy=True)
        y_train = df.loc[train_mask, "vmaf"].to_numpy(dtype=np.float64, copy=True)
        x_val = df.loc[val_mask, list(feature_cols)].to_numpy(dtype=np.float64, copy=True)
        y_val = df.loc[val_mask, "vmaf"].to_numpy(dtype=np.float64, copy=True)
        if x_train.shape[0] == 0 or x_val.shape[0] == 0:
            print(f"  [warn] fold={held_out}: empty split; skipping", file=sys.stderr)
            continue
        if standardize:
            _standardize_inplace(x_train, x_val)
        m = _train_one_fold(
            x_train,
            y_train,
            x_val,
            y_val,
            epochs=epochs,
            batch_size=batch_size,
            lr=lr,
            seed=seed,
        )
        per_fold[held_out] = m
        print(
            f"  fold={held_out:<14} "
            f"PLCC={m['plcc']:.4f}  SROCC={m['srocc']:.4f}  RMSE={m['rmse']:.3f}"
        )
    return per_fold


def _summary(per_fold: dict[str, dict[str, float]]) -> dict[str, float]:
    plccs = [v["plcc"] for v in per_fold.values()]
    sroccs = [v["srocc"] for v in per_fold.values()]
    rmses = [v["rmse"] for v in per_fold.values()]
    return {
        "mean_plcc": float(np.mean(plccs)),
        "std_plcc": float(np.std(plccs, ddof=1)),
        "mean_srocc": float(np.mean(sroccs)),
        "std_srocc": float(np.std(sroccs, ddof=1)),
        "mean_rmse": float(np.mean(rmses)),
        "std_rmse": float(np.std(rmses, ddof=1)),
        "n_folds": len(per_fold),
    }


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(
        prog="phase3_subset_sweep.py",
        description=__doc__,
    )
    ap.add_argument("--parquet", type=Path, required=True)
    ap.add_argument("--out", type=Path, required=True)
    ap.add_argument(
        "--subsets",
        type=str,
        default="canonical6,A,B,C",
        help="Comma-separated subset names from the SUBSETS registry.",
    )
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--seeds",
        type=str,
        default=None,
        help=(
            "Comma-separated seed list for multi-seed validation "
            "(e.g. '0,1,2,3,4'). When set, overrides --seed and "
            "aggregates across the listed seeds. Per Research-0029 "
            "§'Required before shipping' gate 1."
        ),
    )
    ap.add_argument(
        "--standardize",
        action="store_true",
        help=(
            "Per-fold StandardScaler: fit (mean, std) on train, "
            "apply to both train and val. Phase-3b config per "
            "Research-0028 §'Decision'."
        ),
    )
    args = ap.parse_args(raw_argv)

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    print(f"[phase3] parquet={args.parquet} rows={len(df)} cols={len(df.columns)}")
    print(f"[phase3] sources: {sorted(df['source'].unique())}")

    requested = [s.strip() for s in args.subsets.split(",")]
    results: dict[str, dict] = {}
    seeds = [int(s) for s in args.seeds.split(",")] if args.seeds else [args.seed]
    for name in requested:
        if name not in SUBSETS:
            print(f"[phase3] unknown subset {name!r}; valid: {sorted(SUBSETS)}", file=sys.stderr)
            return 2
        feat_cols = SUBSETS[name]
        missing = [c for c in feat_cols if c not in df.columns]
        if missing:
            print(f"[phase3] subset {name}: parquet missing cols {missing}", file=sys.stderr)
            return 2
        print(f"\n=== Subset {name} ({len(feat_cols)} features) ===")
        print(f"  features: {list(feat_cols)}")
        per_seed: dict[int, dict[str, dict[str, float]]] = {}
        for s in seeds:
            print(f"  --- seed={s} ---")
            per_fold = _loso_sweep(
                df,
                feat_cols,
                epochs=args.epochs,
                batch_size=args.batch_size,
                lr=args.lr,
                seed=s,
                standardize=args.standardize,
            )
            per_seed[s] = per_fold
        # Aggregate: mean PLCC etc. across all (seed, fold) pairs.
        flat = [m for fold_map in per_seed.values() for m in fold_map.values()]
        plccs = [m["plcc"] for m in flat]
        sroccs = [m["srocc"] for m in flat]
        rmses = [m["rmse"] for m in flat]
        # Per-seed mean PLCC for seed-only variance.
        seed_means = [
            float(np.mean([m["plcc"] for m in fold_map.values()])) for fold_map in per_seed.values()
        ]
        summary = {
            "mean_plcc": float(np.mean(plccs)),
            "std_plcc": float(np.std(plccs, ddof=1)) if len(plccs) > 1 else 0.0,
            "mean_srocc": float(np.mean(sroccs)),
            "std_srocc": float(np.std(sroccs, ddof=1)) if len(sroccs) > 1 else 0.0,
            "mean_rmse": float(np.mean(rmses)),
            "std_rmse": float(np.std(rmses, ddof=1)) if len(rmses) > 1 else 0.0,
            "n_folds": len(plccs),
            "seed_mean_plcc_std": (
                float(np.std(seed_means, ddof=1)) if len(seed_means) > 1 else 0.0
            ),
            "n_seeds": len(seeds),
        }
        results[name] = {
            "features": list(feat_cols),
            "per_seed": {str(k): v for k, v in per_seed.items()},
            "summary": summary,
        }
        print(
            f"  → mean PLCC={summary['mean_plcc']:.4f}"
            f" (fold-std {summary['std_plcc']:.4f}, "
            f"seed-mean-std {summary['seed_mean_plcc_std']:.4f})  "
            f"SROCC={summary['mean_srocc']:.4f}±{summary['std_srocc']:.4f}  "
            f"RMSE={summary['mean_rmse']:.3f}±{summary['std_rmse']:.3f}"
        )

    # Comparison table
    print(f"\n{'=' * 64}")
    print(
        f"{'Subset':<14} {'Features':>10} {'Mean PLCC':>12} {'± std':>10} {'Δ vs canonical6':>18}"
    )
    print("-" * 64)
    base = results.get("canonical6", {}).get("summary", {}).get("mean_plcc", 0.0)
    for name, r in results.items():
        s = r["summary"]
        delta = s["mean_plcc"] - base if name != "canonical6" else 0.0
        delta_str = "—" if name == "canonical6" else f"{delta:+.4f}"
        print(
            f"{name:<14} {len(r['features']):>10} "
            f"{s['mean_plcc']:>12.4f} {s['std_plcc']:>10.4f} {delta_str:>18}"
        )

    results["run_provenance"] = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=vars(args),
        inputs={"parquet": args.parquet},
        outputs={"json_report": args.out},
    )
    write_manifest_json(args.out, results)
    print(f"\n[phase3] wrote {args.out}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
