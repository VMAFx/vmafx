#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Drive C2 + C3 training on KoNViD-1k frames extracted by
``ai/scripts/extract_konvid_frames.py``.

This is a standalone training driver — it sidesteps the C1-only
``vmaf_train.train`` glue (which assumes a feature-vector parquet) and
plugs the C2 / C3 Lightning models into a frame-loading datamodule
purpose-built for KoNViD MOS / paired-frame data.

Usage::

    python ai/scripts/train_konvid.py --model c2  --epochs 80   --output runs/c2_konvid
    python ai/scripts/train_konvid.py --model c3  --epochs 200  --output runs/c3_konvid
    python ai/scripts/train_konvid.py --model both --epochs-c2 80 --epochs-c3 200

The trained `.ckpt` lands under the run directory; ONNX export and
registry update are separate steps in
``ai/scripts/export_tiny_models.py``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import cast

import pytorch_lightning as L
import torch
from pytorch_lightning.callbacks import EarlyStopping, ModelCheckpoint
from torch.utils.data import DataLoader, Subset

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402
from vmaf_train.data.frame_dataset import FrameMOSDataset, PairedFrameDataset  # noqa: E402
from vmaf_train.data.splits import split_keys  # noqa: E402
from vmaf_train.models import LearnedFilter, NRMetric  # noqa: E402

C2_PARQUET = REPO_ROOT / "ai" / "data" / "konvid_frames_nr.parquet"
C3_PARQUET = REPO_ROOT / "ai" / "data" / "konvid_pairs_filter.parquet"


def _split_dataset(ds, val_frac: float, test_frac: float, seed: int):
    keys = ds.keys
    if keys and len(set(keys)) > 1:
        splits = split_keys(sorted(set(keys)), val_frac, test_frac)
        bag = {
            k: tag
            for tag, kk in (
                ("train", splits.train),
                ("val", splits.val),
                ("test", splits.test),
            )
            for k in kk
        }
        idx_train = [i for i, k in enumerate(keys) if bag[k] == "train"]
        idx_val = [i for i, k in enumerate(keys) if bag[k] == "val"]
        idx_test = [i for i, k in enumerate(keys) if bag[k] == "test"]
    else:
        n = len(ds)
        n_test = int(n * test_frac)
        n_val = int(n * val_frac)
        n_tr = n - n_val - n_test
        gen = torch.Generator().manual_seed(seed)
        perm = torch.randperm(n, generator=gen).tolist()
        idx_train = perm[:n_tr]
        idx_val = perm[n_tr : n_tr + n_val]
        idx_test = perm[n_tr + n_val :]
    return Subset(ds, idx_train), Subset(ds, idx_val), Subset(ds, idx_test)


def _train_one(
    *,
    model: L.LightningModule,
    train_set,
    val_set,
    epochs: int,
    batch_size: int,
    output_dir: Path,
    monitor: str,
    precision: str = "16-mixed",
    seed: int = 0,
) -> Path:
    L.seed_everything(seed, workers=True)
    output_dir.mkdir(parents=True, exist_ok=True)
    train_loader = DataLoader(
        train_set, batch_size=batch_size, shuffle=True, num_workers=4, drop_last=True
    )
    val_loader = DataLoader(val_set, batch_size=batch_size, num_workers=2)
    ckpt_cb = ModelCheckpoint(
        dirpath=output_dir,
        filename="best",
        monitor=monitor,
        mode="min",
        save_top_k=1,
        save_last=True,
    )
    early_cb = EarlyStopping(monitor=monitor, mode="min", patience=15)
    trainer = L.Trainer(
        max_epochs=epochs,
        callbacks=[ckpt_cb, early_cb],
        default_root_dir=output_dir,
        log_every_n_steps=10,
        precision=cast(object, precision),  # type: ignore[arg-type]  # argparse gives str
        deterministic=True,
        accelerator="auto",
    )
    trainer.fit(model, train_loader, val_loader)
    return output_dir / "last.ckpt"


def train_c2(args: argparse.Namespace) -> Path:
    if not C2_PARQUET.exists():
        sys.exit(f"missing {C2_PARQUET}; run ai/scripts/extract_konvid_frames.py first")
    ds = FrameMOSDataset(C2_PARQUET)
    tr, va, _te = _split_dataset(ds, args.val_frac, args.test_frac, args.seed)
    model = NRMetric(in_channels=1, width=args.c2_width)
    output = Path(args.output_c2)
    print(
        f"[c2] training NRMetric on {len(tr)}/{len(va)} train/val frames; " f"width={args.c2_width}"
    )
    return _train_one(
        model=model,
        train_set=tr,
        val_set=va,
        epochs=args.epochs_c2,
        batch_size=args.batch_size_c2,
        output_dir=output,
        monitor="val/mse",
        precision=args.precision,
        seed=args.seed,
    )


def train_c3(args: argparse.Namespace) -> Path:
    if not C3_PARQUET.exists():
        sys.exit(f"missing {C3_PARQUET}; run ai/scripts/extract_konvid_frames.py first")
    ds = PairedFrameDataset(C3_PARQUET)
    tr, va, _te = _split_dataset(ds, args.val_frac, args.test_frac, args.seed)
    model = LearnedFilter(channels=1, width=args.c3_width, num_blocks=args.c3_blocks, lr=1e-4)
    output = Path(args.output_c3)
    print(
        f"[c3] training LearnedFilter on {len(tr)}/{len(va)} train/val pairs; "
        f"width={args.c3_width} blocks={args.c3_blocks}"
    )
    return _train_one(
        model=model,
        train_set=tr,
        val_set=va,
        epochs=args.epochs_c3,
        batch_size=args.batch_size_c3,
        output_dir=output,
        monitor="val/l1",
        precision=args.precision,
        seed=args.seed,
    )


def main(argv: list[str] | None = None) -> int:
    raw_argv = collect_cli_argv(argv)
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--model", choices=("c2", "c3", "both"), default="both")

    parser.add_argument("--epochs-c2", type=int, default=80)
    parser.add_argument("--batch-size-c2", type=int, default=64)
    parser.add_argument("--c2-width", type=int, default=16)
    parser.add_argument("--output-c2", type=Path, default=Path("runs/c2_konvid"))

    parser.add_argument("--epochs-c3", type=int, default=200)
    parser.add_argument("--batch-size-c3", type=int, default=32)
    parser.add_argument("--c3-width", type=int, default=16)
    parser.add_argument("--c3-blocks", type=int, default=4)
    parser.add_argument("--output-c3", type=Path, default=Path("runs/c3_konvid"))

    parser.add_argument("--val-frac", type=float, default=0.1)
    parser.add_argument("--test-frac", type=float, default=0.1)
    parser.add_argument("--seed", type=int, default=0)
    parser.add_argument("--precision", default="16-mixed")
    parser.add_argument(
        "--manifest-out",
        type=Path,
        default=None,
        help=(
            "Run-provenance JSON sidecar (default: <output-c2>/train_konvid.manifest.json "
            "when --model includes c2, else <output-c3>/train_konvid.manifest.json)."
        ),
    )
    args = parser.parse_args(raw_argv)

    checkpoints: dict[str, Path] = {}
    if args.model in ("c2", "both"):
        ck = train_c2(args)
        print(f"[c2] checkpoint: {ck}")
        checkpoints["c2_checkpoint"] = ck
    if args.model in ("c3", "both"):
        ck = train_c3(args)
        print(f"[c3] checkpoint: {ck}")
        checkpoints["c3_checkpoint"] = ck

    # Determine sidecar path and write run-provenance manifest (ADR-0668).
    if args.manifest_out is None:
        anchor = args.output_c2 if args.model in ("c2", "both") else args.output_c3
        args.manifest_out = anchor / "train_konvid.manifest.json"
    outputs = {k: v for k, v in checkpoints.items()}
    outputs["manifest"] = args.manifest_out
    write_manifest_json(
        args.manifest_out,
        {
            "schema": "train-konvid-manifest-v1",
            "model": args.model,
            "run_provenance": build_run_provenance(
                entrypoint=SCRIPT_PATH,
                repo_root=REPO_ROOT,
                argv=raw_argv,
                args=args,
                inputs={
                    "c2_parquet": C2_PARQUET if args.model in ("c2", "both") else None,
                    "c3_parquet": C3_PARQUET if args.model in ("c3", "both") else None,
                },
                outputs=outputs,
            ),
        },
    )
    print(f"[train-konvid] manifest written to {args.manifest_out}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
