#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Combined Netflix + KoNViD-1k tiny-AI training driver.

Concatenates the 9-source Netflix Public corpus
(:class:`ai.train.dataset.NetflixFrameDataset`) with the KoNViD-1k
synthetic-distortion corpus
(:class:`ai.train.konvid_pair_dataset.KoNViDPairDataset`) into one
training matrix. Reuses the model factory, training loop, and ONNX
export from :mod:`ai.train.train` so the two drivers stay in lockstep.

Motivation
----------

Research-0023 §5 identified the FoxBird-class outlier (LOSO PLCC
~0.94 versus 0.99+ on the other 8 sources) as a content-distribution
problem: the Netflix Public corpus does not contain the
high-motion / heavy-grain regime that breaks the linear-regressor
generalisation. Mixing in KoNViD-1k user-generated clips broadens
the content distribution by ~17× clip count (1 200 vs 70) and adds
the UGC + high-motion + low-bitrate regimes that the Netflix corpus
under-represents.

Validation modes
----------------

``--val-mode netflix-source`` (default)
    Hold out one Netflix source (default ``Tennis``) for validation;
    KoNViD is fully in the training set. Mirrors the canonical
    ADR-0203 split so the result is directly comparable to the
    ``mlp_small`` / ``mlp_medium`` baselines from Research-0023.

``--val-mode konvid-holdout``
    Hold out a deterministic 10 % of KoNViD clip keys (no Netflix
    val). Useful for "does this model generalise on UGC at all"
    sanity checks.

``--val-mode netflix-source-and-konvid-holdout``
    Both — validation set is the union. Most rigorous; use this
    once the canonical split numbers are in.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

import numpy as np

if __package__ in (None, ""):
    _here = Path(__file__).resolve()
    _ai_parent = _here.parent.parent.parent  # repo root
    if str(_ai_parent) not in sys.path:
        sys.path.insert(0, str(_ai_parent))
    __package__ = "ai.train"


_DEFAULT_NETFLIX_ROOT = Path(".workingdir2/netflix")
_DEFAULT_KONVID_PARQUET = Path("ai/data/konvid_vmaf_pairs.parquet")
_VAL_MODES = (
    "netflix-source",
    "konvid-holdout",
    "netflix-source-and-konvid-holdout",
    "netflix-only",
    "konvid-only",
)


def build_arg_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="ai/train/train_combined.py",
        description=(
            "Train a tiny-AI model on the union of the Netflix Public "
            "corpus and KoNViD-1k VMAF pairs."
        ),
    )
    p.add_argument("--netflix-root", type=Path, default=_DEFAULT_NETFLIX_ROOT)
    p.add_argument("--konvid-parquet", type=Path, default=_DEFAULT_KONVID_PARQUET)
    p.add_argument(
        "--model-arch",
        choices=("linear", "mlp_small", "mlp_medium"),
        default="mlp_small",
    )
    p.add_argument("--epochs", type=int, default=10)
    p.add_argument("--batch-size", type=int, default=256)
    p.add_argument("--lr", type=float, default=1e-3)
    p.add_argument(
        "--out-dir",
        type=Path,
        default=Path("runs/tiny_combined"),
    )
    p.add_argument(
        "--val-mode",
        choices=_VAL_MODES,
        default="netflix-source",
        help=("How to construct the val split. See module docstring for " "the matrix of options."),
    )
    p.add_argument("--val-source", type=str, default="Tennis")
    p.add_argument(
        "--konvid-val-fraction",
        type=float,
        default=0.1,
        help="Fraction of KoNViD clip keys held out when val-mode includes konvid-holdout.",
    )
    p.add_argument(
        "--max-netflix-pairs",
        type=int,
        default=None,
        help="Cap on Netflix (ref, dis) pairs (smoke).",
    )
    p.add_argument(
        "--max-konvid-clips",
        type=int,
        default=None,
        help="Cap on distinct KoNViD clip keys (smoke).",
    )
    p.add_argument(
        "--assume-dims",
        type=str,
        default=None,
        metavar="WxH",
        help="Stamp every Netflix pair with these dimensions (smoke fixtures only).",
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


def _split_konvid_keys(
    all_keys: tuple[str, ...],
    val_fraction: float,
    seed: int,
) -> tuple[set[str], set[str]]:
    """Deterministic train/val split by clip key."""
    if not all_keys:
        return set(), set()
    rng = np.random.default_rng(seed)
    keys = list(all_keys)
    rng.shuffle(keys)
    n_val = max(1, round(len(keys) * val_fraction))
    val_keys = set(keys[:n_val])
    train_keys = set(keys[n_val:])
    return train_keys, val_keys


def _load_netflix(args, payload_provider):  # type: ignore[no-untyped-def]
    """Build (train_xy, val_xy) for the Netflix slice; ``(empty, empty)`` if disabled."""
    from ..data.feature_extractor import DEFAULT_FEATURES
    from .dataset import NetflixFrameDataset

    empty = (
        np.zeros((0, len(DEFAULT_FEATURES)), dtype=np.float32),
        np.zeros((0,), dtype=np.float32),
    )
    if args.val_mode == "konvid-only":
        return empty, empty
    if not args.netflix_root.is_dir():
        print(
            f"[train_combined] netflix-root {args.netflix_root} missing; " "skipping Netflix slice",
            file=sys.stderr,
        )
        return empty, empty

    assume_dims: tuple[int, int] | None = None
    if args.assume_dims:
        w, h = (int(x) for x in args.assume_dims.lower().split("x"))
        assume_dims = (w, h)

    netflix_holds_val = args.val_mode in (
        "netflix-source",
        "netflix-source-and-konvid-holdout",
        "netflix-only",
    )

    train_ds = NetflixFrameDataset(
        args.netflix_root,
        split="train",
        val_source=args.val_source,
        max_pairs=args.max_netflix_pairs,
        assume_dims=assume_dims,
        payload_provider=payload_provider,
        use_cache=True,
    )
    if netflix_holds_val:
        val_ds = NetflixFrameDataset(
            args.netflix_root,
            split="val",
            val_source=args.val_source,
            max_pairs=args.max_netflix_pairs,
            assume_dims=assume_dims,
            payload_provider=payload_provider,
            use_cache=True,
        )
        val_xy = val_ds.numpy_arrays()
    else:
        val_xy = empty

    print(
        f"[train_combined] netflix train_samples={len(train_ds)} "
        f"val_samples={val_xy[0].shape[0]}"
    )
    return train_ds.numpy_arrays(), val_xy


def _load_konvid(args):  # type: ignore[no-untyped-def]
    """Build (train_xy, val_xy) for the KoNViD slice; ``(empty, empty)`` if disabled."""
    from ..data.feature_extractor import DEFAULT_FEATURES
    from .konvid_pair_dataset import KoNViDPairDataset

    empty = (
        np.zeros((0, len(DEFAULT_FEATURES)), dtype=np.float32),
        np.zeros((0,), dtype=np.float32),
    )
    if args.val_mode == "netflix-only":
        return empty, empty
    if not args.konvid_parquet.is_file():
        print(
            f"[train_combined] konvid-parquet {args.konvid_parquet} missing; "
            "skipping KoNViD slice (run ai/scripts/konvid_to_vmaf_pairs.py first)",
            file=sys.stderr,
        )
        return empty, empty

    full = KoNViDPairDataset(args.konvid_parquet)
    keys = full.unique_keys
    if args.max_konvid_clips is not None and args.max_konvid_clips < len(keys):
        keys = keys[: args.max_konvid_clips]

    konvid_holds_val = args.val_mode in (
        "konvid-holdout",
        "netflix-source-and-konvid-holdout",
        "konvid-only",
    )

    if konvid_holds_val:
        train_keys, val_keys = _split_konvid_keys(keys, args.konvid_val_fraction, args.seed)
    else:
        train_keys = set(keys)
        val_keys = set()

    train_ds = KoNViDPairDataset(args.konvid_parquet, keep_keys=train_keys)
    train_xy = train_ds.numpy_arrays()
    if val_keys:
        val_ds = KoNViDPairDataset(args.konvid_parquet, keep_keys=val_keys)
        val_xy = val_ds.numpy_arrays()
    else:
        val_xy = empty

    print(
        f"[train_combined] konvid train_keys={len(train_keys)} "
        f"val_keys={len(val_keys)} "
        f"train_samples={train_xy[0].shape[0]} val_samples={val_xy[0].shape[0]}"
    )
    return train_xy, val_xy


def _concat_xy(*pairs):  # type: ignore[no-untyped-def]
    xs = [x for x, _ in pairs if x.shape[0] > 0]
    ys = [y for _, y in pairs if y.shape[0] > 0]
    if not xs:
        return pairs[0]
    return np.concatenate(xs, axis=0), np.concatenate(ys, axis=0)


def main(argv: list[str] | None = None) -> int:
    args = build_arg_parser().parse_args(argv)
    args.out_dir.mkdir(parents=True, exist_ok=True)

    try:
        import torch  # noqa: F401
    except ImportError as e:
        print(f"error: PyTorch is required for training: {e}", file=sys.stderr)
        return 2

    from ..data.feature_extractor import DEFAULT_FEATURES
    from .train import _build_model, _train_loop, count_params, export_onnx

    feature_dim = len(DEFAULT_FEATURES)
    module = _build_model(args.model_arch, feature_dim)
    n_params = count_params(module)
    print(
        f"[train_combined] arch={args.model_arch} params={n_params} "
        f"feature_dim={feature_dim} val_mode={args.val_mode}"
    )

    payload_provider = None
    if args.epochs == 0:
        from .dataset import _make_zero_payload

        payload_provider = _make_zero_payload

    netflix_train, netflix_val = _load_netflix(args, payload_provider)
    konvid_train, konvid_val = _load_konvid(args)

    train_xy = _concat_xy(netflix_train, konvid_train)
    val_xy = _concat_xy(netflix_val, konvid_val)
    print(
        f"[train_combined] combined train_samples={train_xy[0].shape[0]} "
        f"val_samples={val_xy[0].shape[0]}"
    )

    if train_xy[0].shape[0] == 0:
        print(
            "[train_combined] no training samples — exporting initial-weights ONNX only",
            file=sys.stderr,
        )
        out = export_onnx(
            module,
            feature_dim,
            args.out_dir / f"{args.model_arch}_combined_final.onnx",
        )
        print(f"[train_combined] exported {out}")
        return 0

    if args.epochs == 0:
        out = export_onnx(
            module,
            feature_dim,
            args.out_dir / f"{args.model_arch}_combined_final.onnx",
        )
        print(f"[train_combined] epochs=0 — exported initial-weights ONNX to {out}")
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
        arch=f"{args.model_arch}_combined",
        feature_dim=feature_dim,
        export_onnx_each=args.export_onnx,
        seed=args.seed,
    ):
        last = ckpt
        print(f"[train_combined] wrote {ckpt}")
    if last is not None:
        print(f"[train_combined] final checkpoint: {last}")
    return 0


if __name__ == "__main__":  # pragma: no cover - exercised via subprocess in tests
    raise SystemExit(main())
