# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Training entry point for tiny-AI on the Netflix corpus.

Lightning-driven; minimal — the heavy lifting (data, eval) lives in the
sibling modules. This file is also the smoke-testable harness:

    python ai/train/train.py --epochs 0 --data-root <fixtures>

CLI args:

* ``--data-root``        directory with ``ref/`` and ``dis/`` (default
                         ``.workingdir2/netflix/``)
* ``--model-arch``       one of ``mlp_small`` / ``mlp_medium`` / ``linear``
* ``--epochs``           training epochs (``0`` runs harness setup only)
* ``--batch-size``       SGD batch size (default 256)
* ``--lr``               learning rate (default 1e-3)
* ``--out-dir``          checkpoint output directory
* ``--val-source``       which source to hold out (default ``Tennis``)
* ``--max-pairs``        cap on (ref, dis) pairs — for unit-testable smoke
* ``--export-onnx-each`` write an ``.onnx`` after every epoch (default on)

Architectures (parameter counts assume ``feature_dim = 6``):

* ``linear``     — 6 -> 1, 7 params
* ``mlp_small``  — 6 -> 16 -> 8 -> 1, ReLU, 257 params
* ``mlp_medium`` — 6 -> 64 -> 32 -> 1, ReLU, 2561 params

ADR-0203 documents the architecture-choice rationale and the
metrics from the first canonical run. CI smoke-tests this script
with ``--epochs 0`` against a mock fixture; full training runs are
invoked directly via ``python ai/train/train.py …`` or the wrapper
``bash ai/scripts/run_training.sh …`` against the local corpus at
``.workingdir2/netflix/``.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path
from typing import Iterable

import numpy as np

# Support both ``python ai/train/train.py`` (script) and
# ``python -m ai.train.train`` (module) invocations. When run as a
# script, ``__package__`` is empty and the ``..`` relative imports
# below would fail; this shim repoints the sys.path so plain-script
# invocation works for the documented smoke-test command.
if __package__ in (None, ""):
    _here = Path(__file__).resolve()
    _ai_parent = _here.parent.parent.parent  # repo root
    if str(_ai_parent) not in sys.path:
        sys.path.insert(0, str(_ai_parent))
    __package__ = "ai.train"  # required for the relative imports below


_DEFAULT_DATA_ROOT = Path(".workingdir2/netflix")


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="ai/train/train.py",
        description="Train a tiny-AI model on the Netflix VMAF corpus.",
    )
    p.add_argument("--data-root", type=Path, default=_DEFAULT_DATA_ROOT)
    p.add_argument(
        "--model-arch",
        choices=("linear", "mlp_small", "mlp_medium"),
        default="mlp_small",
    )
    p.add_argument("--epochs", type=int, default=10)
    p.add_argument("--batch-size", type=int, default=256)
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument("--out-dir", type=Path, default=Path("runs/tiny_nflx"))
    p.add_argument("--val-source", type=str, default="Tennis")
    p.add_argument(
        "--max-pairs",
        type=int,
        default=None,
        help="Optional cap on (ref, dis) pairs — handy for smoke runs.",
    )
    p.add_argument(
        "--assume-dims",
        type=str,
        default=None,
        metavar="WxH",
        help=(
            "Skip the YUV size probe and stamp every pair with these "
            "dimensions. Used by the unit-test smoke fixtures (16x16); "
            "leave unset on the real corpus so size mismatches fail loud."
        ),
    )
    p.add_argument(
        "--no-export-onnx",
        dest="export_onnx",
        action="store_false",
        help="Skip ONNX export after each epoch.",
    )
    p.set_defaults(export_onnx=True)
    p.add_argument("--seed", type=int, default=0)
    return p


def _build_model(arch: str, feature_dim: int):  # type: ignore[no-untyped-def]
    from torch import nn

    if arch == "linear":
        return nn.Sequential(nn.Linear(feature_dim, 1))
    if arch == "mlp_small":
        return nn.Sequential(
            nn.Linear(feature_dim, 16),
            nn.ReLU(),
            nn.Linear(16, 8),
            nn.ReLU(),
            nn.Linear(8, 1),
        )
    if arch == "mlp_medium":
        return nn.Sequential(
            nn.Linear(feature_dim, 64),
            nn.ReLU(),
            nn.Linear(64, 32),
            nn.ReLU(),
            nn.Linear(32, 1),
        )
    raise ValueError(f"unknown arch: {arch}")


def count_params(module) -> int:  # type: ignore[no-untyped-def]
    return int(sum(p.numel() for p in module.parameters() if p.requires_grad))


def export_onnx(  # type: ignore[no-untyped-def]
    module,
    feature_dim: int,
    out_path: Path,
    *,
    opset: int = 17,
) -> Path:
    import torch

    out_path = Path(out_path)
    out_path.parent.mkdir(parents=True, exist_ok=True)
    dummy = torch.zeros((1, feature_dim), dtype=torch.float32)
    module.eval()
    torch.onnx.export(
        module,
        dummy,
        str(out_path),
        input_names=["input"],
        output_names=["score"],
        dynamic_axes={"input": {0: "batch"}, "score": {0: "batch"}},
        opset_version=opset,
    )
    return out_path


def _train_loop(  # type: ignore[no-untyped-def]
    module,
    train_xy: tuple[np.ndarray, np.ndarray],
    val_xy: tuple[np.ndarray, np.ndarray],
    *,
    epochs: int,
    batch_size: int,
    lr: float,
    out_dir: Path,
    arch: str,
    feature_dim: int,
    export_onnx_each: bool,
    seed: int,
) -> Iterable[Path]:
    """Lightweight torch loop. Lightning is overkill for a 7-2561-param net."""
    import torch
    from torch import nn
    from torch.utils.data import DataLoader, TensorDataset

    torch.manual_seed(seed)

    x_tr, y_tr = train_xy
    # val arrays are reserved for future per-epoch eval hooks; not used
    # by the current minimal MSE loop.
    _x_val, _y_val = val_xy

    train_ds = TensorDataset(
        torch.from_numpy(x_tr.astype(np.float32)),
        torch.from_numpy(y_tr.astype(np.float32)).unsqueeze(-1),
    )
    if len(train_ds) == 0:
        # Smoke / empty corpus path — still emit a final ONNX so downstream
        # tooling has a deterministic artefact.
        if export_onnx_each:
            yield export_onnx(module, feature_dim, out_dir / f"{arch}_final.onnx")
        return

    loader = DataLoader(
        train_ds,
        batch_size=batch_size,
        shuffle=True,
        drop_last=False,
    )
    opt = torch.optim.Adam(module.parameters(), lr=lr)
    loss_fn = nn.MSELoss()

    for epoch in range(epochs):
        module.train()
        for xb, yb in loader:
            opt.zero_grad()
            pred = module(xb)
            loss = loss_fn(pred, yb)
            loss.backward()
            opt.step()
        if export_onnx_each:
            yield export_onnx(module, feature_dim, out_dir / f"{arch}_epoch{epoch}.onnx")

    yield export_onnx(module, feature_dim, out_dir / f"{arch}_final.onnx")


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    # Lazy imports so ``--help`` / argparse remains snappy and the module
    # is importable without torch installed.
    try:
        import torch  # noqa: F401
    except ImportError as e:
        print(
            f"error: PyTorch is required for training: {e}",
            file=sys.stderr,
        )
        return 2

    from ..data.feature_extractor import DEFAULT_FEATURES
    from .dataset import NetflixFrameDataset

    feature_dim = len(DEFAULT_FEATURES)
    module = _build_model(args.model_arch, feature_dim)
    n_params = count_params(module)
    print(f"[train] arch={args.model_arch} params={n_params} " f"feature_dim={feature_dim}")

    if not args.data_root.is_dir():
        print(
            f"[train] data-root {args.data_root} does not exist — " "exporting initial ONNX only.",
            file=sys.stderr,
        )
        export_onnx(module, feature_dim, args.out_dir / f"{args.model_arch}_final.onnx")
        return 0

    assume_dims: tuple[int, int] | None = None
    if args.assume_dims:
        try:
            w, h = (int(x) for x in args.assume_dims.lower().split("x"))
        except ValueError as e:
            print(
                f"error: --assume-dims must be WxH (got {args.assume_dims!r}): {e}", file=sys.stderr
            )
            return 2
        assume_dims = (w, h)

    # When ``--epochs 0`` we still want the smoke command to work even
    # without a built ``vmaf`` binary, so inject a zero-filled payload
    # provider that fakes one frame per pair.
    payload_provider = None
    if args.epochs == 0:
        from .dataset import _make_zero_payload

        payload_provider = _make_zero_payload

    train_ds = NetflixFrameDataset(
        args.data_root,
        split="train",
        val_source=args.val_source,
        max_pairs=args.max_pairs,
        assume_dims=assume_dims,
        payload_provider=payload_provider,
        use_cache=False,
    )
    val_ds = NetflixFrameDataset(
        args.data_root,
        split="val",
        val_source=args.val_source,
        max_pairs=args.max_pairs,
        assume_dims=assume_dims,
        payload_provider=payload_provider,
        use_cache=False,
    )
    print(f"[train] train samples={len(train_ds)} val samples={len(val_ds)}")

    train_xy = train_ds.numpy_arrays()
    val_xy = val_ds.numpy_arrays()

    if args.epochs == 0:
        # Smoke path: emit a single initial-weights ONNX and stop.
        out = export_onnx(module, feature_dim, args.out_dir / f"{args.model_arch}_final.onnx")
        print(f"[train] epochs=0 — exported initial-weights ONNX to {out}")
        return 0

    last: Path | None = None
    for ckpt in _train_loop(
        module,
        train_xy,
        val_xy,
        epochs=args.epochs,
        batch_size=args.batch_size,
        lr=args.lr,
        out_dir=args.out_dir,
        arch=args.model_arch,
        feature_dim=feature_dim,
        export_onnx_each=args.export_onnx,
        seed=args.seed,
    ):
        last = ckpt
        print(f"[train] wrote {ckpt}")
    if last is not None:
        print(f"[train] final checkpoint: {last}")
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess in tests
    raise SystemExit(main())
