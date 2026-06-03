# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Codec-id vocabulary + one-hot encoding for the codec-aware FR regressor.

Distortion signatures from x264 (block edges), x265 (CTU-boundary blur),
libsvtav1 (DCT ringing + restoration filters), libvvenc (large CTU
deblocking), and libvpx-vp9 differ systematically. Conditioning the FR
MLP on a codec id lifts cross-codec PLCC/SROCC by 1–3 points on
multi-codec corpora — see the 2026 Bristol VI-Lab review §5.3 cited in
[ADR-0235](../../docs/adr/0235-codec-aware-fr-regressor.md) and
[Research-0040](../../docs/research/0040-codec-aware-fr-conditioning.md).

The vocabulary is closed and ordered. Adding a codec is a schema bump;
existing trained models pin the order they were trained against via the
``codec_vocab`` field in the model-card sidecar, so a re-training is
required when this list grows.

The fallback bucket ``"unknown"`` is mandatory — corpora that ship raw
distorted YUVs without codec metadata (e.g. the Netflix Public corpus
under ``ai/scripts/extract_full_features.py``) tag every clip
``"unknown"`` rather than guessing.
"""

from __future__ import annotations

from typing import Iterable

import numpy as np

# Order is load-bearing — index 0..N-1 matches the one-hot column index
# baked into trained ONNX. Do not reorder; append new entries at the
# end and bump ``CODEC_VOCAB_VERSION``.
CODEC_VOCAB: tuple[str, ...] = (
    "x264",
    "x265",
    "libsvtav1",
    "libvvenc",
    "libvpx-vp9",
    "unknown",
)
CODEC_VOCAB_VERSION = 1
NUM_CODECS = len(CODEC_VOCAB)
UNKNOWN_INDEX = CODEC_VOCAB.index("unknown")


def codec_index(name: str | None) -> int:
    """Return the canonical index for ``name`` or ``UNKNOWN_INDEX`` on miss.

    Empty / None / unrecognised codec names all bucket to ``"unknown"``
    so feature dumps never fail on novel labels — the model still
    receives a deterministic one-hot.
    """
    if name is None:
        return UNKNOWN_INDEX
    name = name.strip().lower()
    if not name:
        return UNKNOWN_INDEX
    # Common alias cleanup so feature-dump scripts that tag from
    # ``ffprobe -show_entries stream=codec_name`` (which emits e.g.
    # "h264", "hevc", "av1", "vp9") map to the encoder bucket the
    # signature characterises.
    aliases = {
        "h264": "x264",
        "avc": "x264",
        "hevc": "x265",
        "h265": "x265",
        "av1": "libsvtav1",
        "vp9": "libvpx-vp9",
        "vvc": "libvvenc",
        "h266": "libvvenc",
    }
    name = aliases.get(name, name)
    if name not in CODEC_VOCAB:
        return UNKNOWN_INDEX
    return CODEC_VOCAB.index(name)


def codec_one_hot(name: str | None, dtype: np.dtype = np.float32) -> np.ndarray:
    """One-hot ``(NUM_CODECS,)`` vector for a single codec label."""
    out = np.zeros(NUM_CODECS, dtype=dtype)
    out[codec_index(name)] = 1.0
    return out


def codec_one_hot_batch(names: Iterable[str | None], dtype: np.dtype = np.float32) -> np.ndarray:
    """One-hot ``(N, NUM_CODECS)`` matrix for a sequence of codec labels."""
    names = list(names)
    out = np.zeros((len(names), NUM_CODECS), dtype=dtype)
    for i, n in enumerate(names):
        out[i, codec_index(n)] = 1.0
    return out
