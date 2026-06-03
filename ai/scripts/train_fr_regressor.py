#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train ``fr_regressor_v1`` (Wave-1 C1 baseline).

T6-1a unblocked: the Netflix Public Dataset is locally available at
``.workingdir2/netflix/`` (lawrence's drop, 9 ref + 70 dis YUVs).
Per-frame full-feature parquet is already produced by
``ai/scripts/extract_full_features.py`` and lives at
``runs/full_features_netflix.parquet`` (11 040 rows × 25 cols).

Pipeline:

  1. Load ``runs/full_features_netflix.parquet``.
  2. 9-fold leave-one-source-out (LOSO) sweep: train ``FRRegressor`` on
     8 sources, evaluate per-frame PLCC / SROCC / RMSE on the held-out
     source. Mean-of-folds is the "held-out" headline number reported
     in the ADR + sidecar.
  3. Refuse to ship if mean LOSO PLCC < 0.95 (ADR-0168 ship gate).
  4. Re-train a final model on **all** 9 sources for the shipping
     checkpoint — the LOSO numbers report generalisation, the shipped
     model uses every available signal.
  5. Export ONNX (opset 17, dynamic batch) and update
     ``model/tiny/registry.json`` + sidecar JSON.

Architecture: stock ``FRRegressor`` from
``ai.src.vmaf_train.models.fr_regressor`` — the Wave-1 spec class
(2-layer GELU MLP, hidden=64, dropout=0.1). Input dim is 6
(canonical-6 features matching ``vmaf_v0.6.1``); deeper subsets (A / B
/ C from Research-0027) can be selected via ``--features``.

Defaults match the published Phase-3 sweep results
(``runs/phase3_subset_sweep.json``), where canonical-6 LOSO PLCC was
0.9845 ± 0.012 — well above the 0.95 ship threshold.

Reproducer (smoke):

  python ai/scripts/train_fr_regressor.py --epochs 3 --max-pairs 4 \\
         --no-export

Reproducer (full):

  python ai/scripts/train_fr_regressor.py
"""

from __future__ import annotations

import argparse
import json
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

from aiutils.file_utils import sha256
from aiutils.run_manifest import build_run_provenance, write_manifest_json

REPO_ROOT = Path(__file__).resolve().parents[2]
SCRIPT_PATH = Path(__file__).resolve()
if str(REPO_ROOT / "ai" / "src") not in sys.path:
    sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))


# Subsets shared with ai/scripts/phase3_subset_sweep.py. We re-declare
# instead of importing so the canonical pool is one file (this script).
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
}


PLCC_SHIP_THRESHOLD = 0.95


def _set_seed(seed: int) -> None:
    import torch

    np.random.seed(seed)
    torch.manual_seed(seed)


def _fit_predict(
    x_train: np.ndarray,
    y_train: np.ndarray,
    x_val: np.ndarray,
    *,
    in_features: int,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
):  # type: ignore[no-untyped-def]
    """Train an :class:`FRRegressor` and return ``(model, val_preds)``."""
    import torch
    from torch.utils.data import DataLoader, TensorDataset

    from vmaf_train.models import FRRegressor

    _set_seed(seed)
    model = FRRegressor(
        in_features=in_features,
        hidden=64,
        depth=2,
        dropout=0.1,
        lr=lr,
        weight_decay=weight_decay,
    )

    ds = TensorDataset(
        torch.from_numpy(x_train.astype(np.float32)),
        torch.from_numpy(y_train.astype(np.float32)),
    )
    loader = DataLoader(ds, batch_size=batch_size, shuffle=True, drop_last=False)
    opt = torch.optim.AdamW(model.parameters(), lr=lr, weight_decay=weight_decay)
    loss_fn = torch.nn.MSELoss()

    model.train()
    for _ep in range(epochs):
        for xb, yb in loader:
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()

    model.eval()
    with torch.no_grad():
        preds = model(torch.from_numpy(x_val.astype(np.float32))).cpu().numpy()
    return model, preds


def _metrics(pred: np.ndarray, target: np.ndarray) -> dict[str, float]:
    pred = pred.astype(np.float64)
    target = target.astype(np.float64)
    if pred.size < 2 or target.size < 2:
        return {"plcc": float("nan"), "srocc": float("nan"), "rmse": float("nan")}
    plcc = float(np.corrcoef(pred, target)[0, 1])
    rmse = float(np.sqrt(np.mean((pred - target) ** 2)))
    rank_p = np.argsort(np.argsort(pred))
    rank_t = np.argsort(np.argsort(target))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"plcc": plcc, "srocc": srocc, "rmse": rmse}


def _standardize(x_train: np.ndarray, x_val: np.ndarray) -> tuple[np.ndarray, dict]:
    mean = x_train.mean(axis=0)
    std = x_train.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_train -= mean
    x_train /= std
    x_val -= mean
    x_val /= std
    return x_val, {"mean": mean.tolist(), "std": std.tolist()}


def _loso_sweep(
    df,
    feature_cols: tuple[str, ...],
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
) -> dict:
    sources = sorted(df["source"].unique())
    per_fold: dict[str, dict[str, float]] = {}
    for held_out in sources:
        train_mask = df["source"] != held_out
        val_mask = df["source"] == held_out
        x_train = df.loc[train_mask, list(feature_cols)].to_numpy(dtype=np.float64)
        y_train = df.loc[train_mask, "vmaf"].to_numpy(dtype=np.float64)
        x_val = df.loc[val_mask, list(feature_cols)].to_numpy(dtype=np.float64)
        y_val = df.loc[val_mask, "vmaf"].to_numpy(dtype=np.float64)
        if x_train.size == 0 or x_val.size == 0:
            print(f"  [warn] fold={held_out}: empty split; skipping", file=sys.stderr)
            continue
        # Per-fold standardisation: fit on train only.
        _, _ = _standardize(x_train, x_val)
        _model, preds = _fit_predict(
            x_train,
            y_train,
            x_val,
            in_features=len(feature_cols),
            epochs=epochs,
            batch_size=batch_size,
            lr=lr,
            weight_decay=weight_decay,
            seed=seed,
        )
        m = _metrics(preds, y_val)
        per_fold[held_out] = m
        print(
            f"  fold={held_out:<14} PLCC={m['plcc']:.4f}  "
            f"SROCC={m['srocc']:.4f}  RMSE={m['rmse']:.3f}"
        )

    plccs = [v["plcc"] for v in per_fold.values()]
    sroccs = [v["srocc"] for v in per_fold.values()]
    rmses = [v["rmse"] for v in per_fold.values()]
    summary = {
        "mean_plcc": float(np.mean(plccs)),
        "std_plcc": float(np.std(plccs, ddof=1)) if len(plccs) > 1 else 0.0,
        "mean_srocc": float(np.mean(sroccs)),
        "std_srocc": float(np.std(sroccs, ddof=1)) if len(sroccs) > 1 else 0.0,
        "mean_rmse": float(np.mean(rmses)),
        "std_rmse": float(np.std(rmses, ddof=1)) if len(rmses) > 1 else 0.0,
        "n_folds": len(per_fold),
    }
    return {"per_fold": per_fold, "summary": summary}


def _train_final(
    df,
    feature_cols: tuple[str, ...],
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    weight_decay: float,
    seed: int,
):  # type: ignore[no-untyped-def]
    """Train on all 9 sources; this is the shipped checkpoint."""
    x = df[list(feature_cols)].to_numpy(dtype=np.float64)
    y = df["vmaf"].to_numpy(dtype=np.float64)
    mean = x.mean(axis=0)
    std = x.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_norm = (x - mean) / std
    model, preds = _fit_predict(
        x_norm,
        y,
        x_norm,  # in-sample; reported as a sanity prediction only
        in_features=len(feature_cols),
        epochs=epochs,
        batch_size=batch_size,
        lr=lr,
        weight_decay=weight_decay,
        seed=seed,
    )
    in_sample = _metrics(preds, y)
    return model, {"feature_mean": mean.tolist(), "feature_std": std.tolist()}, in_sample


def _export_and_register(
    model,  # type: ignore[no-untyped-def]
    *,
    feature_cols: tuple[str, ...],
    standardisation: dict,
    loso_summary: dict,
    in_sample: dict,
    onnx_path: Path,
    sidecar_path: Path,
    registry_path: Path,
    run_provenance: dict[str, Any],
) -> None:
    from vmaf_train.models import export_to_onnx

    in_features = len(feature_cols)
    export_to_onnx(
        model,
        onnx_path,
        in_shape=(1, in_features),
        input_name="features",
        output_name="score",
    )
    digest = sha256(onnx_path)

    notes = (
        "Tiny FR regressor (C1) — 6-feature canonical-6 vector "
        "(adm2, vif_scale0..3, motion2) → VMAF teacher score scalar. "
        "Trained on the Netflix Public Dataset (9 ref + 70 dis YUVs) "
        f"with vmaf_v0.6.1 as DMOS-aligned teacher. "
        f"9-fold LOSO mean PLCC = {loso_summary['mean_plcc']:.4f} ± "
        f"{loso_summary['std_plcc']:.4f}. "
        "Shipped checkpoint trained on all 9 sources. "
        "Exported via ai/scripts/train_fr_regressor.py. See "
        "docs/ai/models/fr_regressor_v1.md + ADR-0249."
    )

    sidecar = {
        "id": "fr_regressor_v1",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
        "input_names": ["features"],
        "output_names": ["score"],
        "feature_order": list(feature_cols),
        "feature_mean": standardisation["feature_mean"],
        "feature_std": standardisation["feature_std"],
        "training": {
            "dataset": "netflix-public",
            "n_sources": 9,
            "n_pairs": 70,
            "loso_mean_plcc": loso_summary["mean_plcc"],
            "loso_std_plcc": loso_summary["std_plcc"],
            "loso_mean_srocc": loso_summary["mean_srocc"],
            "loso_mean_rmse": loso_summary["mean_rmse"],
            "in_sample_plcc": in_sample["plcc"],
        },
        "run_provenance": run_provenance,
    }
    write_manifest_json(sidecar_path, sidecar)

    # Update registry.json. Idempotent: replace any existing
    # 'fr_regressor_v1' row.
    registry = json.loads(registry_path.read_text())
    models = registry.get("models", [])
    new_entry = {
        "id": "fr_regressor_v1",
        "kind": "fr",
        "onnx": onnx_path.name,
        "opset": 17,
        "sha256": digest,
        "notes": notes,
    }
    models = [m for m in models if m.get("id") != "fr_regressor_v1"]
    models.append(new_entry)
    models.sort(key=lambda e: e.get("id", ""))
    registry["models"] = models
    registry_path.write_text(json.dumps(registry, indent=2, sort_keys=True) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(prog="train_fr_regressor.py")
    ap.add_argument(
        "--parquet",
        type=Path,
        default=REPO_ROOT / "runs" / "full_features_netflix.parquet",
    )
    ap.add_argument(
        "--features",
        type=str,
        default="canonical6",
        choices=sorted(SUBSETS),
        help="Feature subset name from SUBSETS (canonical6 | A | B).",
    )
    ap.add_argument("--epochs", type=int, default=30)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--weight-decay", type=float, default=1e-5)
    ap.add_argument("--seed", type=int, default=0)
    ap.add_argument(
        "--ship-threshold",
        type=float,
        default=PLCC_SHIP_THRESHOLD,
        help="Mean LOSO PLCC must exceed this; otherwise refuse to ship.",
    )
    ap.add_argument(
        "--out-onnx",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v1.onnx",
    )
    ap.add_argument(
        "--out-sidecar",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "fr_regressor_v1.json",
    )
    ap.add_argument(
        "--registry",
        type=Path,
        default=REPO_ROOT / "model" / "tiny" / "registry.json",
    )
    ap.add_argument(
        "--metrics-out",
        type=Path,
        default=REPO_ROOT / "runs" / "fr_regressor_v1_metrics.json",
    )
    ap.add_argument(
        "--no-export",
        action="store_true",
        help="Skip ONNX export + registry update (smoke / dev mode).",
    )
    args = ap.parse_args()

    if not args.parquet.is_file():
        print(f"error: parquet not found at {args.parquet}", file=sys.stderr)
        return 2

    run_provenance = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=sys.argv[1:],
        args=args,
        inputs={"parquet": args.parquet},
        outputs={
            "onnx_target": str(args.out_onnx),
            "sidecar_target": str(args.out_sidecar),
            "registry_target": str(args.registry),
            "metrics_target": str(args.metrics_out),
        },
    )

    import pandas as pd

    df = pd.read_parquet(args.parquet)
    feature_cols = SUBSETS[args.features]
    missing = [c for c in feature_cols if c not in df.columns]
    if missing:
        print(f"error: missing feature columns in parquet: {missing}", file=sys.stderr)
        return 2

    print(
        f"[fr-v1] parquet={args.parquet.name} "
        f"rows={len(df)} sources={df['source'].nunique()} "
        f"features={args.features} ({len(feature_cols)} cols)",
        flush=True,
    )

    t0 = time.time()
    print("[fr-v1] running 9-fold LOSO sweep ...", flush=True)
    loso = _loso_sweep(
        df,
        feature_cols,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
    )
    s = loso["summary"]
    print(
        f"[fr-v1] LOSO summary: PLCC={s['mean_plcc']:.4f}±{s['std_plcc']:.4f} "
        f"SROCC={s['mean_srocc']:.4f}±{s['std_srocc']:.4f} "
        f"RMSE={s['mean_rmse']:.3f}±{s['std_rmse']:.3f} "
        f"({s['n_folds']} folds, {time.time() - t0:.0f}s)",
        flush=True,
    )

    if s["mean_plcc"] < args.ship_threshold:
        print(
            f"[fr-v1] REFUSE TO SHIP: mean LOSO PLCC {s['mean_plcc']:.4f} "
            f"< threshold {args.ship_threshold:.4f}. "
            "See docs/ai/models/fr_regressor_v1.md for the diagnosis path.",
            file=sys.stderr,
        )
        # Still emit the metrics JSON for inspection.
        args.metrics_out.parent.mkdir(parents=True, exist_ok=True)
        write_manifest_json(
            args.metrics_out,
            {"loso": loso, "shipped": False, "run_provenance": run_provenance},
        )
        return 3

    print("[fr-v1] training final all-source checkpoint ...", flush=True)
    model, standardisation, in_sample = _train_final(
        df,
        feature_cols,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        weight_decay=args.weight_decay,
        seed=args.seed,
    )
    print(
        f"[fr-v1] final-on-all in-sample PLCC={in_sample['plcc']:.4f} "
        f"SROCC={in_sample['srocc']:.4f} RMSE={in_sample['rmse']:.3f}",
        flush=True,
    )

    metrics_out = {
        "feature_subset": args.features,
        "feature_cols": list(feature_cols),
        "loso": loso,
        "in_sample": in_sample,
        "ship_threshold": args.ship_threshold,
        "shipped": not args.no_export,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "weight_decay": args.weight_decay,
        "seed": args.seed,
        "run_provenance": run_provenance,
    }
    args.metrics_out.parent.mkdir(parents=True, exist_ok=True)
    write_manifest_json(args.metrics_out, metrics_out)
    print(f"[fr-v1] wrote metrics to {args.metrics_out}")

    if args.no_export:
        print("[fr-v1] --no-export set; skipping ONNX export.")
        return 0

    print(
        f"[fr-v1] exporting ONNX → {args.out_onnx} and updating registry {args.registry.name} ...",
        flush=True,
    )
    _export_and_register(
        model,
        feature_cols=feature_cols,
        standardisation=standardisation,
        loso_summary=s,
        in_sample=in_sample,
        onnx_path=args.out_onnx,
        sidecar_path=args.out_sidecar,
        registry_path=args.registry,
        run_provenance=run_provenance,
    )
    print(f"[fr-v1] shipped: {args.out_onnx} (sha256={sha256(args.out_onnx)})")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
