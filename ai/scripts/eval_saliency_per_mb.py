#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Evaluate saliency masks at encoder block granularity.

ADR-0396 Phase 2 needs a metric that matches what ROI encoders consume:
macroblock / CTU decisions after downsampling, not full-resolution
pixel IoU. This utility pairs predicted and ground-truth masks by file
stem, thresholds each block by its mean saliency, and reports per-mask
plus aggregate IoU.

Supported mask formats are intentionally dependency-light:

- ``.npy``: any 2-D numeric array, values are normalised to ``[0, 1]``.
- ``.pgm``: P2 or P5 grayscale masks, values are normalised by maxval.
"""

from __future__ import annotations

import argparse
import json
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Iterable

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

SUPPORTED_SUFFIXES = (".npy", ".pgm")


@dataclass(frozen=True)
class SaliencyIoURow:
    stem: str
    pred_path: str
    gt_path: str
    width: int
    height: int
    block_size: int
    pred_blocks: int
    gt_blocks: int
    intersection_blocks: int
    union_blocks: int
    iou: float


def _strip_pgm_comments(data: bytes) -> list[bytes]:
    lines: list[bytes] = []
    for line in data.splitlines():
        line = line.split(b"#", 1)[0].strip()
        if line:
            lines.append(line)
    return lines


def _read_pgm(path: Path) -> np.ndarray:
    data = path.read_bytes()
    if data.startswith(b"P2"):
        tokens = b" ".join(_strip_pgm_comments(data)).split()
        if len(tokens) < 4:
            raise ValueError(f"PGM header too short: {path}")
        width = int(tokens[1])
        height = int(tokens[2])
        maxval = int(tokens[3])
        values = np.asarray([int(v) for v in tokens[4:]], dtype=np.float32)
        if values.size != width * height:
            raise ValueError(f"PGM payload size mismatch: {path}")
        return (values.reshape(height, width) / float(maxval)).astype(np.float32)

    if not data.startswith(b"P5"):
        raise ValueError(f"unsupported PGM magic in {path}")

    header: list[bytes] = []
    idx = 0
    while len(header) < 4:
        end = data.find(b"\n", idx)
        if end < 0:
            raise ValueError(f"PGM header too short: {path}")
        line = data[idx:end].split(b"#", 1)[0].strip()
        if line:
            header.extend(line.split())
        idx = end + 1
    _, width_raw, height_raw, maxval_raw = header[:4]
    width = int(width_raw)
    height = int(height_raw)
    maxval = int(maxval_raw)
    dtype = np.dtype(np.uint8) if maxval <= 255 else np.dtype(">u2")
    values = np.frombuffer(data[idx:], dtype=dtype, count=width * height)
    if values.size != width * height:
        raise ValueError(f"PGM payload size mismatch: {path}")
    return (values.reshape(height, width).astype(np.float32) / float(maxval)).astype(np.float32)


def load_mask(path: Path) -> np.ndarray:
    """Load a 2-D saliency mask as ``float32 [0, 1]``."""
    if path.suffix == ".npy":
        arr = np.load(path)
    elif path.suffix == ".pgm":
        arr = _read_pgm(path)
    else:
        raise ValueError(f"unsupported mask suffix {path.suffix!r}: {path}")
    arr = np.asarray(arr, dtype=np.float32)
    if arr.ndim != 2:
        raise ValueError(f"mask must be 2-D, got shape {arr.shape}: {path}")
    if not np.isfinite(arr).all():
        raise ValueError(f"mask contains non-finite values: {path}")
    max_value = float(arr.max(initial=0.0))
    if max_value > 1.0:
        arr = arr / max_value
    return np.clip(arr, 0.0, 1.0)


def mask_to_blocks(mask: np.ndarray, block_size: int, threshold: float) -> np.ndarray:
    """Reduce a saliency mask to boolean block decisions."""
    if block_size <= 0:
        raise ValueError(f"block_size must be positive, got {block_size}")
    if not (0.0 <= threshold <= 1.0):
        raise ValueError(f"threshold must be in [0, 1], got {threshold}")
    height, width = mask.shape
    block_h = height // block_size
    block_w = width // block_size
    if block_h == 0 or block_w == 0:
        raise ValueError(f"mask {width}x{height} smaller than block size {block_size}")
    cropped = mask[: block_h * block_size, : block_w * block_size]
    means = cropped.reshape(block_h, block_size, block_w, block_size).mean(axis=(1, 3))
    return means >= threshold


def block_iou(pred: np.ndarray, gt: np.ndarray) -> tuple[int, int, float]:
    """Return ``(intersection, union, iou)`` for boolean block masks."""
    if pred.shape != gt.shape:
        raise ValueError(f"block shapes differ: {pred.shape} != {gt.shape}")
    intersection = int(np.logical_and(pred, gt).sum())
    union = int(np.logical_or(pred, gt).sum())
    iou = 1.0 if union == 0 else float(intersection / union)
    return intersection, union, iou


def _iter_masks(path: Path) -> Iterable[Path]:
    for suffix in SUPPORTED_SUFFIXES:
        yield from sorted(path.glob(f"*{suffix}"))


def pair_masks(pred_dir: Path, gt_dir: Path) -> list[tuple[Path, Path]]:
    """Pair predicted and ground-truth masks by file stem."""
    gt_by_stem = {p.stem: p for p in _iter_masks(gt_dir)}
    pairs: list[tuple[Path, Path]] = []
    for pred in _iter_masks(pred_dir):
        gt = gt_by_stem.get(pred.stem)
        if gt is not None:
            pairs.append((pred, gt))
    if not pairs:
        raise ValueError(f"no paired masks found under {pred_dir} and {gt_dir}")
    return pairs


def evaluate_pair(
    pred_path: Path,
    gt_path: Path,
    *,
    block_size: int,
    threshold: float,
) -> SaliencyIoURow:
    """Evaluate one predicted / ground-truth mask pair."""
    pred = load_mask(pred_path)
    gt = load_mask(gt_path)
    if pred.shape != gt.shape:
        raise ValueError(f"mask shapes differ for {pred_path.stem}: {pred.shape} != {gt.shape}")
    pred_blocks = mask_to_blocks(pred, block_size, threshold)
    gt_blocks = mask_to_blocks(gt, block_size, threshold)
    intersection, union, iou = block_iou(pred_blocks, gt_blocks)
    height, width = pred.shape
    return SaliencyIoURow(
        stem=pred_path.stem,
        pred_path=str(pred_path),
        gt_path=str(gt_path),
        width=int(width),
        height=int(height),
        block_size=int(block_size),
        pred_blocks=int(pred_blocks.sum()),
        gt_blocks=int(gt_blocks.sum()),
        intersection_blocks=intersection,
        union_blocks=union,
        iou=iou,
    )


def evaluate_dirs(
    pred_dir: Path,
    gt_dir: Path,
    *,
    block_size: int,
    threshold: float,
) -> dict[str, object]:
    """Evaluate all paired masks and return a JSON-serialisable payload."""
    rows = [
        evaluate_pair(pred, gt, block_size=block_size, threshold=threshold)
        for pred, gt in pair_masks(pred_dir, gt_dir)
    ]
    total_intersection = sum(row.intersection_blocks for row in rows)
    total_union = sum(row.union_blocks for row in rows)
    macro_iou = float(np.mean([row.iou for row in rows]))
    micro_iou = 1.0 if total_union == 0 else float(total_intersection / total_union)
    return {
        "schema_version": 1,
        "block_size": block_size,
        "threshold": threshold,
        "n_pairs": len(rows),
        "macro_iou": macro_iou,
        "micro_iou": micro_iou,
        "rows": [asdict(row) for row in rows],
    }


def _parse_args(argv: list[str] | None) -> tuple[argparse.Namespace, list[str]]:
    raw_argv = collect_cli_argv(argv)
    parser = make_argument_parser(
        prog="eval_saliency_per_mb.py",
        description=__doc__,
    )
    parser.add_argument("--pred-dir", type=Path, required=True)
    parser.add_argument("--gt-dir", type=Path, required=True)
    parser.add_argument("--block-size", type=int, default=16)
    parser.add_argument("--threshold", type=float, default=0.5)
    parser.add_argument("--out-json", type=Path, default=None)
    return parser.parse_args(raw_argv), raw_argv


def main(argv: list[str] | None = None) -> int:
    args, raw_argv = _parse_args(argv)
    payload = evaluate_dirs(
        args.pred_dir,
        args.gt_dir,
        block_size=args.block_size,
        threshold=args.threshold,
    )
    payload["run_provenance"] = build_run_provenance(
        entrypoint=SCRIPT_PATH,
        repo_root=REPO_ROOT,
        argv=raw_argv,
        args=args,
        inputs={"pred_dir": args.pred_dir, "gt_dir": args.gt_dir},
        outputs={"json_report": args.out_json},
    )
    if args.out_json is None:
        text = json.dumps(payload, indent=2, sort_keys=True) + "\n"
        print(text, end="")
    else:
        write_manifest_json(args.out_json, payload)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
