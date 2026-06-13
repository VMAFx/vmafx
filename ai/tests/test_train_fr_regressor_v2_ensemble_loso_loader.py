# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for ``_load_corpus`` in the ensemble LOSO trainer (ADR-0319).

Covers the canonical-6 schema validation, codec one-hot lookup against
ENCODER_VOCAB v2, and the StandardScaler fit. Synthetic JSONL fixture
keeps the test runtime sub-second on CPU-only CI hosts.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT / "ai" / "scripts"))

# pandas / numpy are training-only deps; skip if unavailable in the
# CPU-only CI install (the doc-only Python lane installs without them).
pd = pytest.importorskip("pandas")
np = pytest.importorskip("numpy")

from train_fr_regressor_v2_ensemble_loso import (  # noqa: E402
    CANONICAL_6,
    CODEC_BLOCK_DIM,
    ENCODER_VOCAB,
    N_ENCODERS,
    UNKNOWN_ENCODER_INDEX,
    _load_corpus,
)


def _write_synthetic_corpus(path: Path, n_per_source: int = 4) -> None:
    """Write a 12-row synthetic JSONL fixture with 3 sources × 4 cqs."""
    sources = ["srcA", "srcB", "srcC"]
    cqs = [19, 25, 31, 37]
    rng = np.random.default_rng(42)
    rows = []
    for src in sources:
        for cq in cqs:
            for f in range(n_per_source):
                rows.append(
                    {
                        "src": src,
                        "encoder": "h264_nvenc",
                        "cq": cq,
                        "frame_index": f,
                        "vmaf": float(95.0 - (cq - 19) * 0.3 + rng.normal(0, 0.5)),
                        "adm2": float(0.95 - (cq - 19) * 0.005),
                        "vif_scale0": float(0.85 - (cq - 19) * 0.003),
                        "vif_scale1": float(0.92 - (cq - 19) * 0.002),
                        "vif_scale2": float(0.97 - (cq - 19) * 0.001),
                        "vif_scale3": float(0.99 - (cq - 19) * 0.0005),
                        "motion2": float(rng.uniform(0.0, 5.0)),
                    }
                )
    with path.open("w", encoding="utf-8") as fh:
        for row in rows:
            fh.write(json.dumps(row) + "\n")


def test_load_corpus_returns_expected_shape(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path, n_per_source=2)
    corpus = _load_corpus(corpus_path)

    assert corpus["target_col"] == "vmaf"
    assert corpus["source_col"] == "src"
    assert corpus["feature_cols"] == list(CANONICAL_6)
    assert len(corpus["codec_block_cols"]) == CODEC_BLOCK_DIM
    assert corpus["codec_block"].shape == (corpus["n_rows"], CODEC_BLOCK_DIM)
    assert corpus["n_rows"] == 3 * 4 * 2  # 3 sources × 4 cqs × 2 frames
    # All h264_nvenc rows should land on the corresponding ENCODER_VOCAB slot.
    nvenc_idx = ENCODER_VOCAB.index("h264_nvenc")
    onehot = corpus["codec_block"][:, :N_ENCODERS]
    assert (onehot[:, nvenc_idx] == 1.0).all()
    assert (onehot.sum(axis=1) == 1.0).all()


def test_load_corpus_scaler_params_in_range(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path)
    corpus = _load_corpus(corpus_path)

    mean = np.asarray(corpus["scaler_params"]["feature_mean"], dtype=np.float64)
    std = np.asarray(corpus["scaler_params"]["feature_std"], dtype=np.float64)
    assert mean.shape == (6,)
    assert std.shape == (6,)
    # ddof=0 std should be > 0 for every canonical-6 column on a
    # non-degenerate corpus; the loader floors near-zero std at 1.0
    # rather than NaN'ing the row.
    assert np.all(std > 0)


def test_load_corpus_crf_norm_in_unit_range(tmp_path: Path) -> None:
    corpus_path = tmp_path / "synth.jsonl"
    _write_synthetic_corpus(corpus_path)
    corpus = _load_corpus(corpus_path)
    crf_norm = corpus["codec_block"][:, -1]
    assert crf_norm.min() == pytest.approx(0.0)
    assert crf_norm.max() == pytest.approx(1.0)
    # Preset column is the constant 0.5 default.
    preset_norm = corpus["codec_block"][:, -2]
    assert (preset_norm == 0.5).all()


def test_load_corpus_unknown_encoder_falls_back(tmp_path: Path) -> None:
    corpus_path = tmp_path / "unknown.jsonl"
    rows = [
        {
            "src": "srcA",
            "encoder": "not_a_real_encoder",
            "cq": 23,
            "frame_index": 0,
            "vmaf": 90.0,
            "adm2": 0.9,
            "vif_scale0": 0.8,
            "vif_scale1": 0.85,
            "vif_scale2": 0.9,
            "vif_scale3": 0.95,
            "motion2": 1.0,
        }
    ]
    with corpus_path.open("w", encoding="utf-8") as fh:
        for r in rows:
            fh.write(json.dumps(r) + "\n")
    corpus = _load_corpus(corpus_path)
    onehot = corpus["codec_block"][:, :N_ENCODERS]
    assert onehot[0, UNKNOWN_ENCODER_INDEX] == 1.0


def test_load_corpus_missing_columns_raises(tmp_path: Path) -> None:
    corpus_path = tmp_path / "bad.jsonl"
    with corpus_path.open("w", encoding="utf-8") as fh:
        fh.write(json.dumps({"src": "s", "encoder": "h264_nvenc", "cq": 23}) + "\n")
    with pytest.raises(ValueError, match="missing required columns"):
        _load_corpus(corpus_path)


def test_load_corpus_missing_file_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError, match="not found"):
        _load_corpus(tmp_path / "does-not-exist.jsonl")
