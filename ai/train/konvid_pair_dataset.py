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

import warnings
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
        # Drop rows whose target or any feature is non-finite (NaN / inf).
        # A NaN slipping into the parquet (failed teacher score, missing
        # feature) would otherwise become a NaN training target and silently
        # poison the regressor's loss. Drop-with-warn rather than propagate.
        finite_cols = [*features, "vmaf"]
        finite_mask = np.isfinite(df[finite_cols].to_numpy(dtype=np.float64, na_value=np.nan)).all(
            axis=1
        )
        n_dropped = int((~finite_mask).sum())
        if n_dropped:
            warnings.warn(
                f"{parquet_path}: dropped {n_dropped} row(s) with non-finite "
                f"vmaf/feature values (NaN/inf) to avoid poisoning training",
                stacklevel=2,
            )
            df = df[finite_mask].reset_index(drop=True)
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
        # Non-finite rows are dropped in __init__; assert here so a future
        # regression that bypasses that filter surfaces loudly instead of
        # silently producing NaN gradients.
        assert (
            np.isfinite(x).all() and np.isfinite(y).all()
        ), "non-finite values reached numpy_arrays() despite __init__ filter"
        return x, y

    @property
    def unique_keys(self) -> tuple[str, ...]:
        """Distinct clip keys in load order — useful for LOSO splits."""
        return tuple(self._df["key"].astype(str).drop_duplicates().tolist())
