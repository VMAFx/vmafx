#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""LOSO eval harness for vmaf_tiny_v5 (mlp_small) on the expanded
5-corpus parquet, with a parallel v2-baseline pass on the 4-corpus
parquet for a same-axes comparison.

For each Netflix ``source`` (9 folds), trains a fresh ``mlp_small``
on the union of the OTHER training rows (v5: 4-corpus minus held-out
NF clip + UGC; v2: 4-corpus minus held-out NF clip), with
corpus-wide StandardScaler fit on those training rows, then evaluates
PLCC / SROCC / RMSE on the held-out NF source. Same protocol as
``eval_loso_vmaf_tiny_v3.py`` but for the v5 (mlp_small + extended
corpus) variant.

Output JSON contains both "v2_baseline" and "v5_extended" sections so
the corpus-expansion delta can be cited directly in the ADR + research
digest.
"""

from __future__ import annotations

import argparse
import sys
import time
from pathlib import Path
from typing import Any

import numpy as np

SCRIPT_PATH = Path(__file__).resolve()
REPO_ROOT = SCRIPT_PATH.parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "src"))
sys.path.insert(0, str(REPO_ROOT))

from ai.scripts.train_vmaf_tiny_v5 import CANONICAL_6, _train  # noqa: E402


def _metrics(pred: np.ndarray, y: np.ndarray) -> dict[str, float]:
    p = pred.astype(np.float64)
    t = y.astype(np.float64)
    plcc = float(np.corrcoef(p, t)[0, 1])
    rmse = float(np.sqrt(np.mean((p - t) ** 2)))
    rank_p = np.argsort(np.argsort(p))
    rank_t = np.argsort(np.argsort(t))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"n": len(y), "plcc": plcc, "srocc": srocc, "rmse": rmse}


def _eval_fold(model, mean, std, x_val, y_val):  # type: ignore[no-untyped-def]
    import torch

    x_std = (x_val - mean) / std
    with torch.no_grad():
        pred = model(torch.from_numpy(x_std.astype(np.float32))).squeeze(-1).numpy()
    return _metrics(pred, y_val)


def _run_loso(df, label: str, epochs: int, batch_size: int, lr: float, seed: int) -> dict:
    nf = df[df["corpus"] == "netflix"]
    sources = sorted(nf["source"].unique().tolist())
    print(f"[{label}] Netflix sources={sources} total_rows={len(df)} nf_rows={len(nf)}", flush=True)
    fold_metrics: dict[str, dict] = {}
    plccs, sroccs, rmses = [], [], []
    for held_out in sources:
        train_mask = ~((df["corpus"] == "netflix") & (df["source"] == held_out))
        val_mask = (df["corpus"] == "netflix") & (df["source"] == held_out)
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
            epochs=epochs,
            batch_size=batch_size,
            lr=lr,
            seed=seed,
        )
        m = _eval_fold(model, mean, std, x_va, y_va)
        fold_metrics[held_out] = m
        plccs.append(m["plcc"])
        sroccs.append(m["srocc"])
        rmses.append(m["rmse"])
        print(
            f"[{label}]   fold={held_out:14s} train={len(x_tr)} val={len(x_va)} "
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
        f"[{label}] aggregate: PLCC={aggregate['mean_plcc']:.4f} ± {aggregate['std_plcc']:.4f} "
        f"SROCC={aggregate['mean_srocc']:.4f} ± {aggregate['std_srocc']:.4f} "
        f"RMSE={aggregate['mean_rmse']:.3f} ± {aggregate['std_rmse']:.3f}",
        flush=True,
    )
    return {"per_fold": fold_metrics, "aggregate": aggregate}


def _validate_and_stamp_teacher(df: Any, name: str, assume_teacher: str | None) -> str:
    if "teacher_model" in df.columns:
        distinct = [str(x) for x in df["teacher_model"].dropna().unique()]
        if len(distinct) > 1:
            raise SystemExit(f"[loso-v5] {name} parquet has mixed teacher_model values: {distinct}")
        if len(distinct) == 0:
            if not assume_teacher:
                raise SystemExit(
                    f"[loso-v5] {name} parquet has empty 'teacher_model'; pass --assume-teacher"
                )
            teacher = assume_teacher
        else:
            teacher = distinct[0]
            if assume_teacher is not None and assume_teacher != teacher:
                raise SystemExit(
                    f"[loso-v5] {name} parquet teacher_model '{teacher}' conflicts with --assume-teacher '{assume_teacher}'"
                )
    else:
        if assume_teacher is None:
            raise SystemExit(
                f"[loso-v5] {name} parquet missing 'teacher_model'; pass --assume-teacher <name> to ingest legacy tables"
            )
        teacher = assume_teacher
    df["teacher_model"] = teacher
    return teacher


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--parquet-base", type=Path, required=True, help="4-corpus parquet (NF+KV+BVI A+B+C+D)."
    )
    ap.add_argument(
        "--parquet-extra", type=Path, required=True, help="UGC parquet (additional rows for v5)."
    )
    ap.add_argument(
        "--assume-teacher",
        default=None,
        help="Teacher model name to assume when ingesting legacy tables lacking a 'teacher_model' column.",
    )
    ap.add_argument("--out-json", type=Path, required=True)
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args(argv)

    import pandas as pd

    base = pd.read_parquet(args.parquet_base)
    extra = pd.read_parquet(args.parquet_extra)
    teacher_base = _validate_and_stamp_teacher(base, "base", args.assume_teacher)
    teacher_extra = _validate_and_stamp_teacher(extra, "extra", args.assume_teacher)
    if teacher_base != teacher_extra:
        raise SystemExit(
            f"[loso-v5] conflicting teacher models: base={teacher_base} extra={teacher_extra}"
        )
    if "corpus" not in extra.columns:
        extra["corpus"] = "ugc"

    common_cols = [*list(CANONICAL_6), "teacher_model", "vmaf", "corpus", "source"]
    base = (
        base[[c for c in common_cols if c in base.columns]]
        .dropna(subset=[*list(CANONICAL_6), "vmaf"])
        .reset_index(drop=True)
    )
    extra = (
        extra[[c for c in common_cols if c in extra.columns]]
        .dropna(subset=[*list(CANONICAL_6), "vmaf"])
        .reset_index(drop=True)
    )
    combined = pd.concat([base, extra], ignore_index=True, sort=False)

    print(
        f"[loso-v5] base_rows={len(base)} extra_rows={len(extra)} "
        f"combined_rows={len(combined)}",
        flush=True,
    )

    t0 = time.monotonic()
    print("\n=== v2 baseline (mlp_small on 4-corpus) ===", flush=True)
    v2_result = _run_loso(base, "v2", args.epochs, args.batch_size, args.lr, args.seed)
    print("\n=== v5 candidate (mlp_small on 5-corpus) ===", flush=True)
    v5_result = _run_loso(combined, "v5", args.epochs, args.batch_size, args.lr, args.seed)
    print(f"\n[loso-v5] total wall = {time.monotonic() - t0:.0f}s", flush=True)

    # Decision rule: v5 wins if mean_plcc improvement >= 1 sigma of v2
    delta_plcc = v5_result["aggregate"]["mean_plcc"] - v2_result["aggregate"]["mean_plcc"]
    sigma = v2_result["aggregate"]["std_plcc"]
    decision = "ship_v5" if delta_plcc >= sigma else "defer"
    print(
        f"[loso-v5] decision: v5_PLCC - v2_PLCC = {delta_plcc:+.4f}  "
        f"(v2_sigma={sigma:.4f}) -> {decision}",
        flush=True,
    )

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    report = {
        "teacher_model": teacher_base,
        "arch": "mlp_small",
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "seed": args.seed,
        "v2_baseline": v2_result,
        "v5_extended": v5_result,
        "delta_plcc": delta_plcc,
        "v2_sigma_plcc": sigma,
        "decision": decision,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=sys.argv[1:],
            args=args,
            inputs={
                "parquet_base": args.parquet_base,
                "parquet_extra": args.parquet_extra,
            },
            outputs={"report_target": str(args.out_json)},
        ),
    }
    write_manifest_json(args.out_json, report)
    print(f"[loso-v5] wrote {args.out_json}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
