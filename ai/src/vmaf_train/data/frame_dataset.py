# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Frame-loading datasets for C2 (NR) and C3 (learned filter) training.

Both expect a parquet produced by ``ai/scripts/extract_konvid_frames.py``:

  * C2 parquet schema: ``key, frame_path, mos`` (one row per clip).
  * C3 parquet schema: ``key, deg_path, clean_path`` (one row per clip; pair
    is degraded → clean for self-supervised residual training).

Frames are stored as uint8 ``.npy`` (HxW, single luma channel) — the
loader normalises to ``float32`` in [0, 1] and adds a leading channel
dim to produce ``(1, H, W)`` tensors compatible with the C2 + C3 models.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd
import torch
from torch.utils.data import Dataset

__all__ = ["FrameMOSDataset", "PairedFrameDataset"]


def _load_frame(path: str | Path) -> torch.Tensor:
    # ``allow_pickle=False`` refuses object-array .npy files, which is the
    # only ``np.load`` code path that can execute arbitrary code on load.
    # The training pipeline writes plain uint8 luma arrays, so this is a
    # tightening rather than a behaviour change for trusted inputs; for
    # untrusted .npy under VMAF_DATA_ROOT it closes a pickled-class
    # remote-code-execution gap.
    arr = np.load(path, allow_pickle=False).astype(np.float32) / 255.0
    return torch.from_numpy(arr).unsqueeze(0)  # (1, H, W)


class FrameMOSDataset(Dataset):
    """C2 — single-frame luma → scalar MOS.

    Returns ``(frame[1,H,W], mos[scalar])`` per item.
    """

    def __init__(self, parquet: str | Path) -> None:
        df = pd.read_parquet(parquet)
        for col in ("frame_path", "mos"):
            if col not in df.columns:
                raise ValueError(f"{parquet}: missing required column {col!r}")
        self._frames: list[str] = df["frame_path"].astype(str).tolist()
        self._mos: list[float] = df["mos"].astype(float).tolist()
        self._keys: list[str] = df["key"].astype(str).tolist() if "key" in df.columns else []

    def __len__(self) -> int:
        return len(self._frames)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, torch.Tensor]:
        x = _load_frame(self._frames[idx])
        y = torch.tensor(self._mos[idx], dtype=torch.float32)
        return x, y

    @property
    def keys(self) -> list[str]:
        return self._keys


class PairedFrameDataset(Dataset):
    """C3 — degraded → clean paired frames (self-supervised L1 residual).

    Returns ``(degraded[1,H,W], clean[1,H,W])`` per item.
    """

    def __init__(self, parquet: str | Path) -> None:
        df = pd.read_parquet(parquet)
        for col in ("deg_path", "clean_path"):
            if col not in df.columns:
                raise ValueError(f"{parquet}: missing required column {col!r}")
        self._deg: list[str] = df["deg_path"].astype(str).tolist()
        self._clean: list[str] = df["clean_path"].astype(str).tolist()
        self._keys: list[str] = df["key"].astype(str).tolist() if "key" in df.columns else []

    def __len__(self) -> int:
        return len(self._deg)

    def __getitem__(self, idx: int) -> tuple[torch.Tensor, torch.Tensor]:
        deg = _load_frame(self._deg[idx])
        clean = _load_frame(self._clean[idx])
        return deg, clean

    @property
    def keys(self) -> list[str]:
        return self._keys
