#!/usr/bin/env python3
# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Train ``saliency_student_v2`` from scratch on DUTS-TR.

This is a fork of ``train_saliency_student.py`` (v1) with **one**
architectural change: the decoder upsampling path swaps the
``nn.ConvTranspose2d`` blocks for the standard "Resize + Conv"
pattern — ``F.interpolate(mode='bilinear', align_corners=False)``
followed by a 3x3 ``nn.Conv2d``. Every other architectural decision
(encoder backbone, channel widths, skip connections, loss function,
training schedule, optimizer, augmentation, seed) is held identical
to v1 so the ablation is clean.

Background
----------
v1 (ADR-0286, shipped 2026-05-03) deliberately avoided ``Resize``
because the ONNX op-allowlist
(``core/src/dnn/op_allowlist.c``) did not yet admit it.
ADR-0258 (accepted 2026-05-03) widened the allowlist to include
``Resize``. v2 exercises the new affordance and tests whether the
"Resize + Conv" pattern — the standard U-Net upsampling shape used by
the broader image-segmentation literature — matches or beats v1's
held-out IoU.

The op-allowlist policy in ADR-0258 enforces op-type only, not
attributes. We export with ``mode='linear'``,
``coordinate_transformation_mode='half_pixel'``, ``antialias=0`` per
the recipe documented in the model card; ORT executes whatever the
graph declares.

Architecture (deltas from v1 marked with [v2])
----------------------------------------------
::

    in                  3 ch  HxW
    enc1   Conv-BN-ReLU x2   3->16, MaxPool             16 ch H/2
    enc2   Conv-BN-ReLU x2  16->32, MaxPool             32 ch H/4
    enc3   Conv-BN-ReLU x2  32->48, MaxPool             48 ch H/8
    bot    Conv-BN-ReLU     48->48                      48 ch H/8
    [v2] up3   Resize x2  +  Conv 3x3 48->32             32 ch H/4
    dec3   Conv-BN-ReLU     80->32 (cat enc3)           32 ch H/4
    [v2] up2   Resize x2  +  Conv 3x3 32->16             16 ch H/2
    dec2   Conv-BN-ReLU     48->16 (cat enc2)           16 ch H/2
    [v2] up1   Resize x2  +  Conv 3x3 16->8               8 ch HxW
    dec1   Conv-BN-ReLU     24->8  (cat enc1)            8 ch HxW
    head   Conv 1x1 8->1, Sigmoid                       1 ch HxW

Loss = BCE + Dice. Best checkpoint by validation IoU is exported to
ONNX opset 17.

I/O contract — identical to v1, drop-in for ``feature_mobilesal.c``::

    inputs:
      input         float32[1, 3, H, W]   ImageNet-normalised RGB, NCHW
    outputs:
      saliency_map  float32[1, 1, H, W]   per-pixel saliency in [0, 1]

Usage
-----
::

    .venv/bin/python ai/scripts/train_saliency_student_v2.py \\
        --duts-root ~/datasets/duts/DUTS-TR \\
        --output    model/tiny/saliency_student_v2.onnx \\
        --epochs    50 --batch-size 32 --lr 1e-3 --seed 42

The script is deterministic given a fixed ``--seed`` and PyTorch /
CUDA versions.
"""

from __future__ import annotations

import argparse
import hashlib
import random
import sys
import time
from dataclasses import dataclass
from pathlib import Path

import numpy as np
import torch
import torch.nn as nn
import torch.nn.functional as F
from PIL import Image
from torch.utils.data import DataLoader, Dataset

try:
    from _script_bootstrap import bootstrap_ai_script
except ModuleNotFoundError:
    from ai.scripts._script_bootstrap import bootstrap_ai_script

_SCRIPT_PATHS = bootstrap_ai_script(__file__)
SCRIPT_PATH = _SCRIPT_PATHS.script_path
REPO_ROOT = _SCRIPT_PATHS.repo_root

from aiutils.cli_helpers import collect_cli_argv, make_argument_parser  # noqa: E402
from aiutils.run_manifest import build_run_provenance, write_manifest_json  # noqa: E402

IMAGENET_MEAN = np.array([0.485, 0.456, 0.406], dtype=np.float32)
IMAGENET_STD = np.array([0.229, 0.224, 0.225], dtype=np.float32)


# ---------------------------------------------------------------------------
# Architecture
# ---------------------------------------------------------------------------


def _conv_bn_relu(c_in: int, c_out: int) -> nn.Sequential:
    return nn.Sequential(
        nn.Conv2d(c_in, c_out, kernel_size=3, padding=1, bias=False),
        nn.BatchNorm2d(c_out),
        nn.ReLU(inplace=True),
    )


class _ResizeConv(nn.Module):
    """Bilinear-resize-by-2 followed by a 3x3 Conv (no bias).

    Replaces the v1 ``ConvTranspose2d(kernel=2, stride=2)`` upsampler.
    The bilinear ``F.interpolate`` call exports to ONNX as a ``Resize``
    op with ``mode='linear'`` and
    ``coordinate_transformation_mode='half_pixel'``.
    """

    def __init__(self, c_in: int, c_out: int) -> None:
        super().__init__()
        self.conv = nn.Conv2d(c_in, c_out, kernel_size=3, padding=1, bias=False)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        x = F.interpolate(x, scale_factor=2.0, mode="bilinear", align_corners=False)
        return self.conv(x)


class TinyUNetV2(nn.Module):
    """Tiny encoder-decoder for binary saliency. v2: Resize+Conv decoder."""

    def __init__(self) -> None:
        super().__init__()
        # Encoder — IDENTICAL to v1.
        self.enc1a = _conv_bn_relu(3, 16)
        self.enc1b = _conv_bn_relu(16, 16)
        self.pool1 = nn.MaxPool2d(2, 2)

        self.enc2a = _conv_bn_relu(16, 32)
        self.enc2b = _conv_bn_relu(32, 32)
        self.pool2 = nn.MaxPool2d(2, 2)

        self.enc3a = _conv_bn_relu(32, 48)
        self.enc3b = _conv_bn_relu(48, 48)
        self.pool3 = nn.MaxPool2d(2, 2)

        # Bottleneck — IDENTICAL to v1.
        self.bot = _conv_bn_relu(48, 48)

        # Decoder — v2 swap. Channel arithmetic identical to v1:
        #   up3 brings H/8 -> H/4, concat with e3 (48ch at H/4) -> 32+48 = 80
        #   up2 brings H/4 -> H/2, concat with e2 (32ch at H/2) -> 16+32 = 48
        #   up1 brings H/2 -> HxW, concat with e1 (16ch at HxW) -> 8+16  = 24
        self.up3 = _ResizeConv(48, 32)
        self.dec3 = _conv_bn_relu(80, 32)

        self.up2 = _ResizeConv(32, 16)
        self.dec2 = _conv_bn_relu(48, 16)

        self.up1 = _ResizeConv(16, 8)
        self.dec1 = _conv_bn_relu(24, 8)

        self.head = nn.Conv2d(8, 1, kernel_size=1)

    def forward(self, x: torch.Tensor) -> torch.Tensor:
        e1 = self.enc1b(self.enc1a(x))  # 16ch HxW
        p1 = self.pool1(e1)  # 16ch H/2
        e2 = self.enc2b(self.enc2a(p1))  # 32ch H/2
        p2 = self.pool2(e2)  # 32ch H/4
        e3 = self.enc3b(self.enc3a(p2))  # 48ch H/4
        p3 = self.pool3(e3)  # 48ch H/8

        b = self.bot(p3)  # 48ch H/8

        u3 = self.up3(b)  # 32ch H/4
        d3 = self.dec3(torch.cat([u3, e3], dim=1))  # -> 32ch H/4

        u2 = self.up2(d3)  # 16ch H/2
        d2 = self.dec2(torch.cat([u2, e2], dim=1))  # -> 16ch H/2

        u1 = self.up1(d2)  # 8ch HxW
        d1 = self.dec1(torch.cat([u1, e1], dim=1))  # -> 8ch HxW

        return torch.sigmoid(self.head(d1))


def count_parameters(model: nn.Module) -> int:
    return sum(p.numel() for p in model.parameters() if p.requires_grad)


# ---------------------------------------------------------------------------
# Data — copied verbatim from v1 to keep the ablation clean.
# ---------------------------------------------------------------------------


@dataclass
class DutsItem:
    image_path: Path
    mask_path: Path


def _scan_duts_tr(root: Path) -> list[DutsItem]:
    """Walk DUTS-TR/{DUTS-TR-Image,DUTS-TR-Mask} and pair files."""
    img_dir = root / "DUTS-TR-Image"
    mask_dir = root / "DUTS-TR-Mask"
    if not img_dir.is_dir() or not mask_dir.is_dir():
        raise FileNotFoundError(
            f"Expected DUTS-TR layout at {root}: missing DUTS-TR-Image/ or DUTS-TR-Mask/"
        )
    items: list[DutsItem] = []
    for img in sorted(img_dir.glob("*.jpg")):
        mask = mask_dir / (img.stem + ".png")
        if mask.is_file():
            items.append(DutsItem(image_path=img, mask_path=mask))
    if not items:
        raise FileNotFoundError(f"No (image, mask) pairs found under {root}")
    return items


class DutsDataset(Dataset):
    """DUTS-TR with random crop+flip augmentation, ImageNet normalisation."""

    def __init__(
        self,
        items: list[DutsItem],
        crop_size: int = 256,
        train: bool = True,
    ) -> None:
        self.items = items
        self.crop_size = crop_size
        self.train = train

    def __len__(self) -> int:
        return len(self.items)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, torch.Tensor]:
        it = self.items[idx]
        img = Image.open(it.image_path).convert("RGB")
        mask = Image.open(it.mask_path).convert("L")

        target_short = self.crop_size + 32 if self.train else self.crop_size
        w0, h0 = img.size
        if w0 < h0:
            new_w = target_short
            new_h = round(h0 * (target_short / w0))
        else:
            new_h = target_short
            new_w = round(w0 * (target_short / h0))
        img = img.resize((new_w, new_h), Image.Resampling.BILINEAR)
        mask = mask.resize((new_w, new_h), Image.Resampling.NEAREST)

        if self.train:
            x = random.randint(0, new_w - self.crop_size)
            y = random.randint(0, new_h - self.crop_size)
            img = img.crop((x, y, x + self.crop_size, y + self.crop_size))
            mask = mask.crop((x, y, x + self.crop_size, y + self.crop_size))
            if random.random() < 0.5:
                img = img.transpose(Image.FLIP_LEFT_RIGHT)
                mask = mask.transpose(Image.FLIP_LEFT_RIGHT)
        else:
            x = (new_w - self.crop_size) // 2
            y = (new_h - self.crop_size) // 2
            img = img.crop((x, y, x + self.crop_size, y + self.crop_size))
            mask = mask.crop((x, y, x + self.crop_size, y + self.crop_size))

        arr = np.asarray(img, dtype=np.float32) / 255.0
        arr = (arr - IMAGENET_MEAN) / IMAGENET_STD
        arr = arr.transpose(2, 0, 1)
        img_t = torch.from_numpy(np.ascontiguousarray(arr))

        m = np.asarray(mask, dtype=np.float32) / 255.0
        m = (m > 0.5).astype(np.float32)
        mask_t = torch.from_numpy(m).unsqueeze(0)

        return img_t, mask_t


# ---------------------------------------------------------------------------
# Loss + metrics
# ---------------------------------------------------------------------------


def bce_dice_loss(pred: torch.Tensor, target: torch.Tensor, eps: float = 1.0) -> torch.Tensor:
    bce = F.binary_cross_entropy(pred, target)
    inter = (pred * target).sum(dim=(1, 2, 3))
    union = pred.sum(dim=(1, 2, 3)) + target.sum(dim=(1, 2, 3))
    dice = 1.0 - (2.0 * inter + eps) / (union + eps)
    return bce + dice.mean()


def iou_score(pred: torch.Tensor, target: torch.Tensor, threshold: float = 0.5) -> float:
    """Mean IoU over the batch at a fixed threshold."""
    p = (pred > threshold).float()
    t = (target > 0.5).float()
    inter = (p * t).sum(dim=(1, 2, 3))
    union = p.sum(dim=(1, 2, 3)) + t.sum(dim=(1, 2, 3)) - inter
    iou = (inter + 1e-6) / (union + 1e-6)
    return float(iou.mean().item())


# ---------------------------------------------------------------------------
# Training loop
# ---------------------------------------------------------------------------


def train_epoch(
    model: nn.Module,
    loader: DataLoader,
    opt: torch.optim.Optimizer,
    device: torch.device,
) -> float:
    model.train()
    total = 0.0
    n_batches = 0
    for img, mask in loader:
        img = img.to(device, non_blocking=True)
        mask = mask.to(device, non_blocking=True)
        opt.zero_grad(set_to_none=True)
        pred = model(img)
        loss = bce_dice_loss(pred, mask)
        loss.backward()
        opt.step()
        total += float(loss.item())
        n_batches += 1
    return total / max(n_batches, 1)


@torch.no_grad()
def validate(
    model: nn.Module,
    loader: DataLoader,
    device: torch.device,
) -> tuple[float, float]:
    model.eval()
    total_loss = 0.0
    total_iou = 0.0
    n = 0
    for img, mask in loader:
        img = img.to(device, non_blocking=True)
        mask = mask.to(device, non_blocking=True)
        pred = model(img)
        total_loss += float(bce_dice_loss(pred, mask).item())
        total_iou += iou_score(pred, mask)
        n += 1
    return total_loss / max(n, 1), total_iou / max(n, 1)


# ---------------------------------------------------------------------------
# ONNX export
# ---------------------------------------------------------------------------


def export_onnx(model: nn.Module, output: Path, opset: int = 17) -> None:
    """Export the model to ONNX. Caller must hand a CPU model in eval mode."""
    model.eval()
    dummy = torch.zeros(1, 3, 256, 256, dtype=torch.float32)
    output.parent.mkdir(parents=True, exist_ok=True)
    torch.onnx.export(
        model,
        dummy,
        str(output),
        input_names=["input"],
        output_names=["saliency_map"],
        dynamic_axes={
            "input": {2: "H", 3: "W"},
            "saliency_map": {2: "H", 3: "W"},
        },
        opset_version=opset,
        do_constant_folding=True,
        training=torch.onnx.TrainingMode.EVAL,
        dynamo=False,
    )


def parity_check(
    model: nn.Module, onnx_path: Path, *, threshold: float = 1e-5, seed: int = 0
) -> float:
    """Compare PyTorch (CPU eval) vs ONNX Runtime on a fixed random input."""
    import onnxruntime as ort

    model_cpu = model.cpu().eval()
    rng = np.random.RandomState(seed)
    x = rng.randn(1, 3, 256, 256).astype(np.float32)
    with torch.no_grad():
        y_pt = model_cpu(torch.from_numpy(x)).numpy()
    sess = ort.InferenceSession(str(onnx_path), providers=["CPUExecutionProvider"])
    y_ort = sess.run(["saliency_map"], {"input": x})[0]
    diff = float(np.max(np.abs(y_pt - y_ort)))
    if diff > threshold:
        raise RuntimeError(
            f"PyTorch <-> ONNX parity FAILED: max-abs-diff={diff:.3e}  "
            f"threshold={threshold:.0e}"
        )
    return diff


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------


def set_seed(seed: int) -> None:
    random.seed(seed)
    np.random.seed(seed)
    torch.manual_seed(seed)
    torch.cuda.manual_seed_all(seed)


def _build_metrics_payload(
    *,
    best_val_iou: float,
    param_count: int,
    args: argparse.Namespace,
    history: list[dict],
    total_time_sec: float,
    onnx_bytes: int,
    onnx_sha256: str,
    pt_onnx_max_abs_diff: float,
    device: torch.device,
    run_provenance: dict[str, object] | None = None,
) -> dict[str, object]:
    payload: dict[str, object] = {
        "best_val_iou": best_val_iou,
        "param_count": param_count,
        "epochs": args.epochs,
        "batch_size": args.batch_size,
        "lr": args.lr,
        "crop_size": args.crop_size,
        "seed": args.seed,
        "opset": args.opset,
        "history": history,
        "total_time_sec": total_time_sec,
        "onnx_bytes": onnx_bytes,
        "onnx_sha256": onnx_sha256,
        "pt_onnx_max_abs_diff": pt_onnx_max_abs_diff,
        "device": str(device),
    }
    if run_provenance is not None:
        payload["run_provenance"] = run_provenance
    return payload


def main(argv: list[str] | None = None) -> int:
    parser = make_argument_parser(
        prog="train_saliency_student_v2.py",
        description=__doc__,
    )
    parser.add_argument("--duts-root", type=Path, required=True)
    parser.add_argument(
        "--output",
        type=Path,
        required=True,
        help="Path to write the trained ONNX, e.g. model/tiny/saliency_student_v2.onnx",
    )
    parser.add_argument("--epochs", type=int, default=50)
    parser.add_argument("--batch-size", type=int, default=32)
    parser.add_argument("--lr", type=float, default=1e-3)
    parser.add_argument("--crop-size", type=int, default=256)
    parser.add_argument("--val-fraction", type=float, default=0.05)
    parser.add_argument("--num-workers", type=int, default=4)
    parser.add_argument("--seed", type=int, default=42)
    parser.add_argument("--opset", type=int, default=17)
    parser.add_argument("--device", default="cuda" if torch.cuda.is_available() else "cpu")
    parser.add_argument(
        "--metrics-out", type=Path, default=None, help="Optional JSON file to dump training metrics"
    )
    raw_argv = collect_cli_argv(argv)
    args = parser.parse_args(raw_argv)

    set_seed(args.seed)

    items = _scan_duts_tr(args.duts_root)
    print(f"DUTS-TR pairs found: {len(items)}", flush=True)
    rng = random.Random(args.seed)
    rng.shuffle(items)
    n_val = max(64, round(args.val_fraction * len(items)))
    val_items = items[:n_val]
    train_items = items[n_val:]
    print(f"split: train={len(train_items)}  val={len(val_items)}", flush=True)

    train_ds = DutsDataset(train_items, crop_size=args.crop_size, train=True)
    val_ds = DutsDataset(val_items, crop_size=args.crop_size, train=False)

    train_loader = DataLoader(
        train_ds,
        batch_size=args.batch_size,
        shuffle=True,
        num_workers=args.num_workers,
        pin_memory=True,
        drop_last=True,
        persistent_workers=args.num_workers > 0,
    )
    val_loader = DataLoader(
        val_ds,
        batch_size=args.batch_size,
        shuffle=False,
        num_workers=args.num_workers,
        pin_memory=True,
        persistent_workers=args.num_workers > 0,
    )

    device = torch.device(args.device)
    model = TinyUNetV2().to(device)
    n_params = count_parameters(model)
    print(f"model: TinyUNetV2  trainable params = {n_params}", flush=True)
    if n_params > 200_000:
        print(f"WARN: param count {n_params} exceeds 200K target", flush=True)

    opt = torch.optim.Adam(model.parameters(), lr=args.lr)
    sched = torch.optim.lr_scheduler.CosineAnnealingLR(opt, T_max=args.epochs)

    best_iou = -1.0
    best_state: dict | None = None
    history: list[dict] = []
    t0 = time.time()
    for epoch in range(1, args.epochs + 1):
        ep_t = time.time()
        train_loss = train_epoch(model, train_loader, opt, device)
        val_loss, val_iou = validate(model, val_loader, device)
        sched.step()
        elapsed = time.time() - ep_t
        line = (
            f"epoch {epoch:02d}/{args.epochs}  "
            f"train_loss={train_loss:.4f}  val_loss={val_loss:.4f}  "
            f"val_iou={val_iou:.4f}  ({elapsed:.1f}s)"
        )
        print(line, flush=True)
        history.append(
            {
                "epoch": epoch,
                "train_loss": train_loss,
                "val_loss": val_loss,
                "val_iou": val_iou,
                "elapsed_sec": elapsed,
            }
        )
        if val_iou > best_iou:
            best_iou = val_iou
            best_state = {k: v.detach().cpu().clone() for k, v in model.state_dict().items()}
            print(f"  -> new best val_iou={best_iou:.4f}", flush=True)

    total_time = time.time() - t0
    print(f"training done. best val_iou={best_iou:.4f}  total={total_time:.1f}s", flush=True)

    if best_state is None:
        print("FATAL: no checkpoint was saved (training did not run)", file=sys.stderr)
        return 1
    model.load_state_dict(best_state)

    model = model.cpu().eval()
    export_onnx(model, args.output, opset=args.opset)
    onnx_bytes = args.output.read_bytes()
    digest = hashlib.sha256(onnx_bytes).hexdigest()
    print(f"exported {args.output}  ({len(onnx_bytes)} bytes  sha256={digest})", flush=True)

    diff = parity_check(model, args.output, threshold=1e-5, seed=args.seed)
    print(f"PT <-> ORT parity max-abs-diff = {diff:.3e}  (threshold 1e-5)", flush=True)

    if args.metrics_out is not None:
        write_manifest_json(
            args.metrics_out,
            _build_metrics_payload(
                best_val_iou=best_iou,
                param_count=n_params,
                args=args,
                history=history,
                total_time_sec=total_time,
                onnx_bytes=len(onnx_bytes),
                onnx_sha256=digest,
                pt_onnx_max_abs_diff=diff,
                device=device,
                run_provenance=build_run_provenance(
                    entrypoint=SCRIPT_PATH,
                    repo_root=REPO_ROOT,
                    argv=raw_argv,
                    args=args,
                    inputs={"duts_root": args.duts_root},
                    outputs={"onnx": args.output, "metrics": args.metrics_out},
                ),
            ),
        )
        print(f"wrote metrics -> {args.metrics_out}", flush=True)

    return 0


if __name__ == "__main__":
    sys.exit(main())
