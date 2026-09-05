# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for :class:`ai.train.dataset.NetflixFrameDataset`.

Mocks libvmaf via the ``payload_provider`` injection point so the suite
runs without a built ``vmaf`` binary or the real corpus.
"""

from __future__ import annotations

from pathlib import Path

import numpy as np
import pytest

torch = pytest.importorskip("torch")

from ai.data.feature_extractor import DEFAULT_FEATURES  # noqa: E402
from ai.data.scores import resolve_teacher_model  # noqa: E402
from ai.train.dataset import DEFAULT_VAL_SOURCE, NetflixFrameDataset  # noqa: E402


def _make_payload(pair, n_frames=3, seed=0):  # type: ignore[no-untyped-def]
    rng = np.random.default_rng(seed)
    feat = rng.standard_normal((n_frames, len(DEFAULT_FEATURES))).astype(np.float32)
    score = rng.uniform(0, 100, size=n_frames).astype(np.float32)
    return {
        "features": {
            "feature_names": list(DEFAULT_FEATURES),
            "per_frame": feat.tolist(),
            "n_frames": int(n_frames),
        },
        "scores": {
            "per_frame": score.tolist(),
            "pooled": float(score.mean()),
            "teacher_model": resolve_teacher_model().name,
        },
    }


def test_dataset_train_split_excludes_val_source(
    mock_corpus: Path, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    ds = NetflixFrameDataset(
        mock_corpus,
        split="train",
        val_source="BetaSrc",
        payload_provider=lambda p: _make_payload(p, n_frames=2, seed=hash(p.cache_key) & 0xFF),
        assume_dims=(16, 16),
    )
    assert len(ds) == 4  # 2 AlphaSrc dis × 2 frames
    sources = {s.source for s in ds._samples}
    assert sources == {"AlphaSrc"}


def test_dataset_val_split_only_val_source(mock_corpus: Path, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    ds = NetflixFrameDataset(
        mock_corpus,
        split="val",
        val_source="BetaSrc",
        payload_provider=lambda p: _make_payload(p, n_frames=2, seed=42),
        assume_dims=(16, 16),
    )
    assert len(ds) == 4  # 2 BetaSrc dis × 2 frames
    sources = {s.source for s in ds._samples}
    assert sources == {"BetaSrc"}


def test_dataset_default_val_source_is_tennis() -> None:
    assert DEFAULT_VAL_SOURCE == "Tennis"


def test_dataset_returns_tensor_with_correct_shape(
    mock_corpus: Path, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    ds = NetflixFrameDataset(
        mock_corpus,
        split="train",
        val_source="BetaSrc",
        payload_provider=lambda p: _make_payload(p, n_frames=3, seed=0),
        assume_dims=(16, 16),
    )
    x, y = ds[0]
    assert x.shape == (len(DEFAULT_FEATURES),)
    assert x.dtype == torch.float32
    assert y.dtype == torch.float32


def test_dataset_numpy_arrays_round_trip(mock_corpus: Path, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    ds = NetflixFrameDataset(
        mock_corpus,
        split="train",
        val_source="BetaSrc",
        payload_provider=lambda p: _make_payload(p, n_frames=5, seed=0),
        assume_dims=(16, 16),
    )
    X, y = ds.numpy_arrays()
    assert X.shape == (len(ds), len(DEFAULT_FEATURES))
    assert y.shape == (len(ds),)
    assert X.dtype == np.float32
    assert y.dtype == np.float32


def test_dataset_caches_payloads(mock_corpus: Path, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    calls = {"n": 0}

    def provider(p):
        calls["n"] += 1
        return _make_payload(p, n_frames=2, seed=0)

    NetflixFrameDataset(
        mock_corpus,
        split="train",
        val_source="BetaSrc",
        payload_provider=provider,
        assume_dims=(16, 16),
    )
    first_calls = calls["n"]
    NetflixFrameDataset(
        mock_corpus,
        split="train",
        val_source="BetaSrc",
        payload_provider=provider,
        assume_dims=(16, 16),
    )
    assert calls["n"] == first_calls, "Second instantiation must hit the cache."


def test_dataset_recomputes_cache_from_other_or_unknown_teacher(
    mock_corpus: Path, tmp_path: Path, monkeypatch
) -> None:
    """ADR-1173: a cache entry stamped with another teacher, or a legacy entry
    with no stamp at all, is stale -- it is recomputed, never relabelled."""
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    calls = {"n": 0}

    def provider(p):
        calls["n"] += 1
        return _make_payload(p, n_frames=2, seed=0)

    def build():
        return NetflixFrameDataset(
            mock_corpus,
            split="train",
            val_source="BetaSrc",
            payload_provider=provider,
            assume_dims=(16, 16),
        )

    build()
    first_calls = calls["n"]
    assert first_calls > 0

    from ai.data.netflix_loader import cache_path_for, iter_pairs

    cache_files = [
        cache_path_for(p)
        for p in iter_pairs(mock_corpus, assume_dims=(16, 16))
        if p.source != "BetaSrc"
    ]
    assert cache_files and all(f.is_file() for f in cache_files)

    # (a) stamped with a different teacher -> every entry is recomputed.
    import json

    for f in cache_files:
        payload = json.loads(f.read_text())
        payload["scores"]["teacher_model"] = "vmaf_v0.6.1"
        f.write_text(json.dumps(payload))
    build()
    assert calls["n"] == 2 * first_calls

    # (b) legacy entry without any stamp -> also recomputed.
    for f in cache_files:
        payload = json.loads(f.read_text())
        del payload["scores"]["teacher_model"]
        f.write_text(json.dumps(payload))
    build()
    assert calls["n"] == 3 * first_calls

    # (c) freshly written entries carry the resolved teacher and are reused.
    build()
    assert calls["n"] == 3 * first_calls
