# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""KoNViD-1k VMAF-pair dataset adapter.

Loads the parquet produced by ``ai/scripts/konvid_to_vmaf_pairs.py``
and exposes the same interface as
:class:`ai.train.dataset.NetflixFrameDataset` so the LOSO trainer
can ingest KoNViD-1k pairs alongside (or instead of) the
9-source Netflix Public corpus.

This addresses Research-0023 §5: the existing Netflix Public
corpus is fully utilised; the FoxBird-class content-distribution
variance needs a *different / larger* training corpus to address.
KoNViD-1k is the natural starting point — 1 200 user-generated
clips at 540p with synthetic-distortion FR pairs (libx264 CRF=35
round-trip; same recipe used for the Netflix dis-pairs in the
existing corpus).

Schema expected from the parquet (produced by the acquisition
script):

  key           : str  (KoNViD clip identifier)
  frame_index   : int  (per-clip frame number)
  vif_scale0..3 : float
  adm2          : float
  motion2       : float
  vmaf          : float (vmaf_v0.6.1 teacher score)

The dataset can be filtered by ``val_clips`` / ``train_clips`` for
LOSO-style holdouts: caller passes the set of clip keys to keep.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd

try:
    import torch
    from torch.utils.data import Dataset

    _HAS_TORCH = True
except ImportError:  # pragma: no cover
    _HAS_TORCH = False

    class Dataset:  # type: ignore[no-redef]
        pass


from ..data.feature_extractor import DEFAULT_FEATURES

__all__ = ["KoNViDPairDataset"]


class KoNViDPairDataset(Dataset):  # type: ignore[misc]
    """LOSO-trainer-compatible KoNViD-1k VMAF-pair dataset.

    Drops into the same trainer as :class:`NetflixFrameDataset` —
    same ``feature_dim`` (6), same ``numpy_arrays() → (X, y)`` shape
    so the existing :func:`ai.train.train._train_loop` consumes it
    without modification.

    Parameters
    ----------
    parquet_path:
        Path to the parquet produced by
        ``ai/scripts/konvid_to_vmaf_pairs.py``.
    keep_keys:
        Optional set / list of KoNViD clip keys to retain. ``None``
        means "all". For LOSO holdouts, the caller filters
        per-fold (e.g. for cross-corpus eval the held-out KoNViD
        subset becomes the val split).
    features:
        Feature column order — must match the trainer's expected
        feature order. Defaults to the ``vmaf_v0.6.1`` 6-feature
        set, identical to ``NetflixFrameDataset``.
    """

    def __init__(
        self,
        parquet_path: Path | str,
        *,
        keep_keys: set[str] | list[str] | None = None,
        features: tuple[str, ...] = DEFAULT_FEATURES,
    ) -> None:
        df = pd.read_parquet(parquet_path)
        for col in (*features, "key", "frame_index", "vmaf"):
            if col not in df.columns:
                raise ValueError(f"{parquet_path}: missing required column {col!r}")
        if keep_keys is not None:
            keep_keys = set(keep_keys)
            df = df[df["key"].isin(keep_keys)].reset_index(drop=True)
        self._df = df
        self.features = features
        self.keys = df["key"].astype(str).tolist()

    def __len__(self) -> int:
        return len(self._df)

    def __getitem__(self, idx: int):  # type: ignore[no-untyped-def]
        row = self._df.iloc[idx]
        feats = np.asarray(
            [float(row[f]) for f in self.features],
            dtype=np.float32,
        )
        target = float(row["vmaf"])
        if _HAS_TORCH:
            x = torch.from_numpy(feats)
            y = torch.tensor(target, dtype=torch.float32)
            return x, y
        return feats, np.float32(target)

    @property
    def feature_dim(self) -> int:
        return len(self.features)

    def numpy_arrays(self) -> tuple[np.ndarray, np.ndarray]:
        """Stack samples into ``(X, y)`` for the trainer's batch loop."""
        if self._df.empty:
            return (
                np.zeros((0, self.feature_dim), dtype=np.float32),
                np.zeros((0,), dtype=np.float32),
            )
        x = self._df[list(self.features)].to_numpy(dtype=np.float32)
        y = self._df["vmaf"].to_numpy(dtype=np.float32)
        return x, y

    @property
    def unique_keys(self) -> tuple[str, ...]:
        """Distinct clip keys in load order — useful for LOSO splits."""
        return tuple(self._df["key"].astype(str).drop_duplicates().tolist())
