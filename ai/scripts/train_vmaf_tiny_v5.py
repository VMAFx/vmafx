#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train ``vmaf_tiny_v5`` — mlp_small on expanded corpus (4-corpus + UGC).

Architecturally identical to ``vmaf_tiny_v2`` (mlp_small, 6 → 16 → 8 → 1,
~257 params). The only delta vs v2 is the training corpus:
v2 = NF + KoNViD + BVI-DVC A+B+C+D (4-corpus, 330 499 rows);
v5 = 4-corpus + YouTube UGC vp9 subset (5-corpus). Same hyperparams as
v2 — 90 epochs, Adam lr=1e-3, MSE, batch_size 256, StandardScaler baked
into the ONNX graph (ADR-0216 trust-root).

The script loads two parquet inputs and concatenates them on the
shared canonical-6 + ``vmaf`` columns. Other columns (cambi, ssimulacra2,
etc.) may be NaN in the UGC parquet and are dropped before training —
v5 only consumes canonical-6.

Output: PyTorch checkpoint + JSON sidecar with scaler stats, identical
schema to ``train_vmaf_tiny_v2`` so the existing ``export_vmaf_tiny_v2``
exporter can be reused.
"""

from __future__ import annotations

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

CANONICAL_6: tuple[str, ...] = (
    "adm2",
    "vif_scale0",
    "vif_scale1",
    "vif_scale2",
    "vif_scale3",
    "motion2",
)


def _build_mlp_small(in_dim: int):  # type: ignore[no-untyped-def]
    from torch import nn

    return nn.Sequential(
        nn.Linear(in_dim, 16),
        nn.ReLU(),
        nn.Linear(16, 8),
        nn.ReLU(),
        nn.Linear(8, 1),
    )


def _train(x, y, *, epochs, batch_size, lr, seed):  # type: ignore[no-untyped-def]
    import torch
    from torch import nn

    torch.manual_seed(seed)
    in_dim = x.shape[1]
    model = _build_mlp_small(in_dim)
    opt = torch.optim.Adam(model.parameters(), lr=lr)
    loss_fn = nn.MSELoss()

    x_t = torch.from_numpy(x.astype(np.float32))
    y_t = torch.from_numpy(y.astype(np.float32)).unsqueeze(-1)
    n = x_t.shape[0]
    rng = np.random.default_rng(seed)
    for ep in range(epochs):
        model.train()
        perm = rng.permutation(n)
        running = 0.0
        n_batches = 0
        for start in range(0, n, batch_size):
            idx = torch.from_numpy(perm[start : start + batch_size])
            xb = x_t.index_select(0, idx)
            yb = y_t.index_select(0, idx)
            opt.zero_grad()
            loss = loss_fn(model(xb), yb)
            loss.backward()
            opt.step()
            running += float(loss.item())
            n_batches += 1
        if (ep + 1) % 10 == 0 or ep == 0:
            print(
                f"  epoch {ep + 1:3d}/{epochs}  train_mse={running / max(1, n_batches):.4f}",
                flush=True,
            )
    return model.eval()


def _train_metrics(model, x: np.ndarray, y: np.ndarray) -> dict[str, float]:
    import torch

    with torch.no_grad():
        preds = model(torch.from_numpy(x.astype(np.float32))).squeeze(-1).numpy()
    pred = preds.astype(np.float64)
    target = y.astype(np.float64)
    plcc = float(np.corrcoef(pred, target)[0, 1])
    rmse = float(np.sqrt(np.mean((pred - target) ** 2)))
    rank_p = np.argsort(np.argsort(pred))
    rank_t = np.argsort(np.argsort(target))
    srocc = float(np.corrcoef(rank_p, rank_t)[0, 1])
    return {"plcc": plcc, "srocc": srocc, "rmse": rmse}


def _load(parquet: Path, name: str):  # type: ignore[no-untyped-def]
    import pandas as pd

    df = pd.read_parquet(parquet)
    miss = [c for c in CANONICAL_6 if c not in df.columns]
    if miss:
        raise SystemExit(f"[train-v5] {name} parquet missing columns: {miss}")
    if "vmaf" not in df.columns:
        raise SystemExit(f"[train-v5] {name} parquet missing 'vmaf' column")
    keep = [*list(CANONICAL_6), "vmaf"]
    if "corpus" in df.columns:
        keep = ["corpus", *keep]
    if "source" in df.columns:
        keep = ["source", *keep]
    return df[keep]


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    ap = make_argument_parser(prog="train_vmaf_tiny_v5.py", description=__doc__)
    ap.add_argument(
        "--parquet-base",
        type=Path,
        required=True,
        help="Existing 4-corpus parquet (NF+KV+BVI A+B+C+D).",
    )
    ap.add_argument(
        "--parquet-extra",
        type=Path,
        required=True,
        help="UGC parquet from extract_ugc_features.py.",
    )
    ap.add_argument("--out-ckpt", type=Path, required=True)
    ap.add_argument("--out-stats", type=Path, required=True)
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    args = ap.parse_args(raw_argv)

    import pandas as pd
    import torch

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    base = _load(args.parquet_base, "base")
    extra = _load(args.parquet_extra, "extra")
    if "corpus" not in extra.columns:
        extra["corpus"] = "ugc"
    df = pd.concat([base, extra], ignore_index=True, sort=False)
    df = df.dropna(subset=[*list(CANONICAL_6), "vmaf"]).reset_index(drop=True)
    print(
        f"[train-v5] base_rows={len(base)} extra_rows={len(extra)} "
        f"combined_rows={len(df)} "
        f"corpora={df.get('corpus', pd.Series(['<none>'])).value_counts().to_dict()}",
        flush=True,
    )

    x = df[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    y = df["vmaf"].to_numpy(dtype=np.float64)

    mean = x.mean(axis=0)
    std = x.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    x_std = (x - mean) / std
    print(f"[train-v5] mean={mean.round(4).tolist()}\n           std ={std.round(4).tolist()}")
    print(f"[train-v5] training mlp_small for {args.epochs} epochs (lr={args.lr})", flush=True)
    model = _train(
        x_std, y, epochs=args.epochs, batch_size=args.batch_size, lr=args.lr, seed=args.seed
    )
    metrics = _train_metrics(model, x_std, y)
    print(
        f"[train-v5] train metrics: PLCC={metrics['plcc']:.4f} "
        f"SROCC={metrics['srocc']:.4f} RMSE={metrics['rmse']:.3f}",
        flush=True,
    )

    args.out_ckpt.parent.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "state_dict": model.state_dict(),
            "features": list(CANONICAL_6),
            "input_mean": mean.tolist(),
            "input_std": std.tolist(),
            "train_metrics": metrics,
            "epochs": args.epochs,
            "lr": args.lr,
            "batch_size": args.batch_size,
            "seed": args.seed,
        },
        args.out_ckpt,
    )
    stats_payload = {
        "features": list(CANONICAL_6),
        "input_mean": mean.tolist(),
        "input_std": std.tolist(),
        "train_metrics": metrics,
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "seed": args.seed,
        "n_train_rows": len(df),
        "parquet_base": str(args.parquet_base),
        "parquet_extra": str(args.parquet_extra),
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={
                "parquet_base": args.parquet_base,
                "parquet_extra": args.parquet_extra,
            },
            outputs={
                "checkpoint_target": str(args.out_ckpt),
                "stats_target": str(args.out_stats),
            },
        ),
    }
    write_manifest_json(args.out_stats, stats_payload)
    print(f"[train-v5] wrote {args.out_ckpt} and {args.out_stats}")
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
