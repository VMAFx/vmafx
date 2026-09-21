#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Train ``vmaf_tiny_v4`` — wider/deeper tiny VMAF MLP (mlp_large).

Same recipe as ``train_vmaf_tiny_v2`` / ``train_vmaf_tiny_v3`` except
for the architecture function. Architecture: ``mlp_large``
(6 → 64 → 32 → 16 → 1, ~2 705 params) — roughly 3.5x the parameter
count of v3's ``mlp_medium`` (769) and ~10.5x v2's ``mlp_small``
(257). Goal is to test whether the extra capacity buys headroom over
v3's PLCC=0.9986 ± 0.0015 baseline on Netflix LOSO.

* Features: canonical-6 = ``(adm2, vif_scale0..3, motion2)``.
* Preprocessing: corpus-wide StandardScaler. The exporter bakes
  ``mean`` / ``std`` into the ONNX graph as Constant nodes so the
  runtime feeds raw feature values (same trust-root as v2/v3).
* Optimiser: Adam @ lr=1e-3, MSE loss, 90 epochs, batch_size 256
  (identical to v2/v3 — only the arch changes).

Output is a torch checkpoint + a JSON sidecar with the scaler
statistics, both consumed by ``export_vmaf_tiny_v4.py``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import TYPE_CHECKING

import numpy as np

if TYPE_CHECKING:
    import torch

    from ai.scripts._script_bootstrap import bootstrap_ai_script
else:
    try:
        from ai.scripts._script_bootstrap import bootstrap_ai_script
    except ModuleNotFoundError:
        from _script_bootstrap import bootstrap_ai_script

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


def _build_mlp_large(in_dim: int) -> "torch.nn.Module":
    """v4 architecture — 6 → 64 → 32 → 16 → 1, ~2 705 params.

    ~3.5x the hidden capacity of ``_build_mlp_medium`` (v3). Linear-1
    has ``in_dim*64 + 64`` = 448 params, Linear-2 has ``64*32 + 32``
    = 2 080 params, Linear-3 has ``32*16 + 16`` = 528 params,
    Linear-4 has ``16*1 + 1`` = 17 params; total 3 073 when in_dim=6
    (the spec's 2.7K estimate is an underestimate — the exact count
    is irrelevant; what matters is the >3x capacity bump over v3).
    """
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


def _train(
    x: np.ndarray,
    y: np.ndarray,
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    seed: int,
) -> "torch.nn.Module":
    """Train mlp_large on the standardised feature matrix.

    Identical loop to v2/v3 — only the model factory differs.
    """
    import torch
    from torch import nn

    torch.manual_seed(seed)
    in_dim = x.shape[1]
    model = _build_mlp_large(in_dim)
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


def _train_metrics(model: "torch.nn.Module", x: np.ndarray, y: np.ndarray) -> dict[str, float]:
    """Compute PLCC / SROCC / RMSE on the standardised training set."""
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


def _count_params(model: "torch.nn.Module") -> int:
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


def _parse_args(raw_argv: list[str]) -> argparse.Namespace:
    ap = make_argument_parser(prog="train_vmaf_tiny_v4.py", description=__doc__)
    ap.add_argument(
        "--parquet",
        type=Path,
        required=True,
        help="Full-feature parquet (e.g. 4-corpus: Netflix + KoNViD + BVI-DVC A+B+C+D).",
    )
    ap.add_argument(
        "--out-ckpt",
        type=Path,
        required=True,
        help="Output PyTorch checkpoint (.pt) — state_dict + scaler stats.",
    )
    ap.add_argument(
        "--out-stats",
        type=Path,
        required=True,
        help=(
            "Output JSON with {features, input_mean, input_std, train_metrics} "
            "consumed by export_vmaf_tiny_v4.py."
        ),
    )
    ap.add_argument("--epochs", type=int, default=90)
    ap.add_argument("--batch-size", type=int, default=256)
    ap.add_argument("--lr", type=float, default=1e-3)
    ap.add_argument("--seed", type=int, default=0)
    return ap.parse_args(raw_argv)


def _load_training_data(
    args: argparse.Namespace,
) -> tuple[np.ndarray, np.ndarray, np.ndarray, np.ndarray, int] | None:
    import pandas as pd

    frame = pd.read_parquet(args.parquet)
    missing = [column for column in CANONICAL_6 if column not in frame.columns]
    if missing:
        print(f"[train-v4] parquet missing columns: {missing}", file=sys.stderr)
        return None
    if "vmaf" not in frame.columns:
        print("[train-v4] parquet missing 'vmaf' target column", file=sys.stderr)
        return None
    print(f"[train-v4] parquet={args.parquet} rows={len(frame)} " f"features={list(CANONICAL_6)}")
    x = frame[list(CANONICAL_6)].to_numpy(dtype=np.float64)
    y = frame["vmaf"].to_numpy(dtype=np.float64)
    mean = x.mean(axis=0)
    std = x.std(axis=0, ddof=0)
    std = np.where(std < 1e-8, 1.0, std)
    return (x - mean) / std, y, mean, std, len(frame)


def _write_outputs(
    args: argparse.Namespace,
    raw_argv: list[str],
    model: "torch.nn.Module",
    metrics: dict[str, float],
    mean: np.ndarray,
    std: np.ndarray,
    n_rows: int,
    n_params: int,
) -> None:
    import torch

    from aiutils.run_manifest import build_run_provenance, write_manifest_json

    args.out_ckpt.parent.mkdir(parents=True, exist_ok=True)
    torch.save(
        {
            "state_dict": model.state_dict(),
            "features": list(CANONICAL_6),
            "input_mean": mean.tolist(),
            "input_std": std.tolist(),
            "train_metrics": metrics,
            "arch": "mlp_large",
            "n_params": n_params,
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
        "arch": "mlp_large",
        "n_params": n_params,
        "epochs": args.epochs,
        "lr": args.lr,
        "batch_size": args.batch_size,
        "seed": args.seed,
        "n_train_rows": n_rows,
        "run_provenance": build_run_provenance(
            entrypoint=SCRIPT_PATH,
            repo_root=REPO_ROOT,
            argv=raw_argv,
            args=args,
            inputs={"parquet": args.parquet},
            outputs={
                "checkpoint_target": str(args.out_ckpt),
                "stats_target": str(args.out_stats),
            },
        ),
    }
    write_manifest_json(args.out_stats, stats_payload)
    print(f"[train-v4] wrote {args.out_ckpt} and {args.out_stats}")


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    args = _parse_args(raw_argv)
    training_data = _load_training_data(args)
    if training_data is None:
        return 2
    x_std, y, mean, std, n_rows = training_data
    print(f"[train-v4] mean={mean.round(4).tolist()}\n           std ={std.round(4).tolist()}")
    print(f"[train-v4] training mlp_large for {args.epochs} epochs (lr={args.lr})")
    model = _train(
        x_std,
        y,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        seed=args.seed,
    )
    n_params = _count_params(model)
    metrics = _train_metrics(model, x_std, y)
    print(
        f"[train-v4] arch=mlp_large n_params={n_params}  "
        f"train metrics: PLCC={metrics['plcc']:.4f} "
        f"SROCC={metrics['srocc']:.4f} RMSE={metrics['rmse']:.3f}"
    )
    _write_outputs(args, raw_argv, model, metrics, mean, std, n_rows, n_params)
    return 0


if __name__ == "__main__":  # pragma: no cover
    raise SystemExit(main())
