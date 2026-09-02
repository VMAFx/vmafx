# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke test for the combined Netflix + KoNViD-1k trainer.

Verifies:
  * ``--epochs 0`` exports an initial-weights ONNX without touching
    libvmaf, real Netflix corpus, or full KoNViD parquet.
  * The KoNViD split helper produces deterministic train / val key
    sets that are disjoint and cover the input.
  * Empty Netflix + empty KoNViD fast-paths to an initial ONNX.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pandas as pd
import pytest

from ai.data.feature_extractor import DEFAULT_FEATURES
from ai.train.train_combined import _split_konvid_keys, main


def _mock_parquet(path: Path, *, n_keys: int = 4, frames_per_key: int = 3) -> Path:
    rows = []
    rng = np.random.default_rng(0)
    for k in range(n_keys):
        for f in range(frames_per_key):
            row = {feat: float(rng.random()) for feat in DEFAULT_FEATURES}
            row["key"] = f"clip{k:03d}"
            row["frame_index"] = f
            row["vmaf"] = float(50.0 + 10.0 * rng.random())
            rows.append(row)
    df = pd.DataFrame(rows)
    df.to_parquet(path)
    return path


def test_split_konvid_keys_is_disjoint_and_complete():
    train, val = _split_konvid_keys(("a", "b", "c", "d", "e"), val_fraction=0.4, seed=0)
    assert train.isdisjoint(val)
    assert train | val == {"a", "b", "c", "d", "e"}
    assert len(val) == 2  # round(5 * 0.4) = 2


def test_split_konvid_keys_deterministic_under_seed():
    a = _split_konvid_keys(tuple(f"clip{i}" for i in range(20)), val_fraction=0.1, seed=42)
    b = _split_konvid_keys(tuple(f"clip{i}" for i in range(20)), val_fraction=0.1, seed=42)
    assert a == b


def test_split_konvid_keys_empty_input():
    train, val = _split_konvid_keys((), val_fraction=0.1, seed=0)
    assert train == set()
    assert val == set()


def test_combined_trainer_epochs_zero_with_konvid_only(tmp_path: Path):
    pytest.importorskip("torch")
    parquet = _mock_parquet(tmp_path / "konvid.parquet")
    out_dir = tmp_path / "out"
    rc = main(
        [
            "--netflix-root",
            str(tmp_path / "no-such-dir"),
            "--konvid-parquet",
            str(parquet),
            "--epochs",
            "0",
            "--val-mode",
            "konvid-only",
            "--out-dir",
            str(out_dir),
            "--model-arch",
            "linear",
        ]
    )
    assert rc == 0
    artefacts = list(out_dir.glob("*.onnx"))
    assert artefacts, f"no ONNX artefact in {out_dir}: {list(out_dir.iterdir())}"


def test_combined_trainer_no_data_produces_initial_onnx(tmp_path: Path):
    pytest.importorskip("torch")
    out_dir = tmp_path / "out"
    rc = main(
        [
            "--netflix-root",
            str(tmp_path / "no-such-dir"),
            "--konvid-parquet",
            str(tmp_path / "no-such.parquet"),
            "--epochs",
            "0",
            "--out-dir",
            str(out_dir),
            "--model-arch",
            "linear",
        ]
    )
    assert rc == 0
    artefacts = list(out_dir.glob("*.onnx"))
    assert artefacts, f"no ONNX artefact in {out_dir}: {list(out_dir.iterdir())}"
