# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for :class:`ai.train.konvid_pair_dataset.KoNViDPairDataset`.

Builds a synthetic parquet matching the schema the acquisition script
(``ai/scripts/konvid_to_vmaf_pairs.py``) produces, then exercises the
loader's interface — confirming it mirrors
:class:`NetflixFrameDataset` close enough for the LOSO trainer to
swap in.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from ai.data.feature_extractor import DEFAULT_FEATURES
from ai.train.konvid_pair_dataset import KoNViDPairDataset


def _synthetic_parquet(tmp_path: Path, n_clips: int = 3, n_frames: int = 5) -> Path:
    rng = np.random.default_rng(seed=42)
    rows: list[dict] = []
    for c in range(n_clips):
        key = f"KoNViD_1k_videos_{1000 + c}"
        for i in range(n_frames):
            row: dict = {"key": key, "frame_index": i}
            for feat in DEFAULT_FEATURES:
                row[feat] = float(rng.standard_normal())
            row["vmaf"] = float(rng.uniform(20.0, 100.0))
            rows.append(row)
    parquet = tmp_path / "konvid_vmaf_pairs.parquet"
    pd.DataFrame(rows).to_parquet(parquet, index=False)
    return parquet


def test_loader_basic_shape(tmp_path: Path) -> None:
    parquet = _synthetic_parquet(tmp_path, n_clips=3, n_frames=5)
    ds = KoNViDPairDataset(parquet)
    assert len(ds) == 15
    assert ds.feature_dim == len(DEFAULT_FEATURES) == 6
    assert len(ds.unique_keys) == 3
    x, y = ds.numpy_arrays()
    assert x.shape == (15, 6)
    assert y.shape == (15,)
    assert x.dtype == np.float32
    assert y.dtype == np.float32


def test_loader_keep_keys_filter(tmp_path: Path) -> None:
    parquet = _synthetic_parquet(tmp_path, n_clips=4, n_frames=5)
    ds_all = KoNViDPairDataset(parquet)
    val_keys = {ds_all.unique_keys[0]}
    train_keys = set(ds_all.unique_keys) - val_keys
    val_ds = KoNViDPairDataset(parquet, keep_keys=val_keys)
    train_ds = KoNViDPairDataset(parquet, keep_keys=train_keys)
    assert len(val_ds) == 5
    assert len(train_ds) == 15
    assert len(val_ds) + len(train_ds) == len(ds_all)


def test_loader_missing_column_raises(tmp_path: Path) -> None:
    parquet = tmp_path / "broken.parquet"
    pd.DataFrame({"key": ["x"], "frame_index": [0], "vmaf": [50.0]}).to_parquet(
        parquet, index=False
    )
    with pytest.raises(ValueError, match="missing required column"):
        KoNViDPairDataset(parquet)


def test_loader_empty_after_filter(tmp_path: Path) -> None:
    parquet = _synthetic_parquet(tmp_path, n_clips=2, n_frames=3)
    ds = KoNViDPairDataset(parquet, keep_keys={"KoNViD_1k_videos_DOES_NOT_EXIST"})
    assert len(ds) == 0
    x, y = ds.numpy_arrays()
    assert x.shape == (0, 6)
    assert y.shape == (0,)


def test_loader_torch_item_shape(tmp_path: Path) -> None:
    torch = pytest.importorskip("torch")
    parquet = _synthetic_parquet(tmp_path, n_clips=1, n_frames=2)
    ds = KoNViDPairDataset(parquet)
    x, y = ds[0]
    assert isinstance(x, torch.Tensor)
    assert x.shape == (6,)
    assert isinstance(y, torch.Tensor)
    assert y.shape == ()  # scalar tensor
