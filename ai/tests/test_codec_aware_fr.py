# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Codec-aware FR regressor smoke tests.

Covers ai/src/vmaf_train/codec.py + the ``num_codecs`` extension to
``FRRegressor``. We exercise:

  1. The codec vocabulary contract (closed, ordered, includes
     ``"unknown"``).
  2. ``codec_index`` aliases (h264 → x264, hevc → x265, av1 →
     libsvtav1, vp9 → libvpx-vp9, vvc/h266 → libvvenc) and
     ``UNKNOWN_INDEX`` fallback.
  3. ``FRRegressor(num_codecs=0)`` is bit-equivalent to the v1
     contract — accepts a single tensor input.
  4. ``FRRegressor(num_codecs=NUM_CODECS)`` requires the second
     positional input and rejects mis-shaped one-hots.
  5. The 3-tuple ``(x, codec, y)`` batch path produces a finite loss
     and propagates gradients to the codec-conditioned columns.
"""

from __future__ import annotations

import pytest

torch = pytest.importorskip("torch")

from conftest import requires_pytorch_lightning  # noqa: E402

requires_pytorch_lightning()

import numpy as np  # noqa: E402

from vmaf_train.codec import (  # noqa: E402
    CODEC_VOCAB,
    NUM_CODECS,
    UNKNOWN_INDEX,
    codec_index,
    codec_one_hot,
    codec_one_hot_batch,
)


def test_codec_vocab_contract() -> None:
    assert "unknown" in CODEC_VOCAB
    assert CODEC_VOCAB[UNKNOWN_INDEX] == "unknown"
    assert len(CODEC_VOCAB) == NUM_CODECS
    # No duplicates.
    assert len(set(CODEC_VOCAB)) == NUM_CODECS


def test_codec_index_aliases() -> None:
    assert codec_index("x264") == CODEC_VOCAB.index("x264")
    assert codec_index("h264") == CODEC_VOCAB.index("x264")
    assert codec_index("hevc") == CODEC_VOCAB.index("x265")
    assert codec_index("AV1") == CODEC_VOCAB.index("libsvtav1")
    assert codec_index("vp9") == CODEC_VOCAB.index("libvpx-vp9")
    assert codec_index("h266") == CODEC_VOCAB.index("libvvenc")
    # Unknown / missing labels collapse to UNKNOWN_INDEX.
    assert codec_index(None) == UNKNOWN_INDEX
    assert codec_index("") == UNKNOWN_INDEX
    assert codec_index("not-a-codec") == UNKNOWN_INDEX


def test_codec_one_hot_shape_and_sum() -> None:
    v = codec_one_hot("x264")
    assert v.shape == (NUM_CODECS,)
    assert v.dtype == np.float32
    assert v.sum() == 1.0
    assert v[CODEC_VOCAB.index("x264")] == 1.0

    batch = codec_one_hot_batch(["x264", None, "av1", "garbage"])
    assert batch.shape == (4, NUM_CODECS)
    assert (batch.sum(axis=1) == 1.0).all()
    assert batch[1, UNKNOWN_INDEX] == 1.0
    assert batch[3, UNKNOWN_INDEX] == 1.0


def test_fr_regressor_num_codecs_zero_is_v1_contract() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=0).eval()
    x = torch.randn(4, 6)
    with torch.no_grad():
        y = m(x)
    assert y.shape == (4,)


def test_fr_regressor_num_codecs_requires_codec_input() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=NUM_CODECS).eval()
    x = torch.randn(4, 6)
    with pytest.raises(ValueError, match="codec_onehot is required"):
        m(x)


def test_fr_regressor_codec_aware_forward_shape() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=NUM_CODECS).eval()
    x = torch.randn(4, 6)
    codec = torch.tensor(codec_one_hot_batch(["x264", "x265", "libsvtav1", "unknown"]))
    with torch.no_grad():
        y = m(x, codec)
    assert y.shape == (4,)


def test_fr_regressor_codec_dim_mismatch_rejected() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=NUM_CODECS).eval()
    x = torch.randn(4, 6)
    bad = torch.zeros(4, NUM_CODECS + 2)
    with pytest.raises(ValueError, match="codec_onehot last-dim"):
        m(x, bad)


def test_fr_regressor_codec_aware_training_step_finite() -> None:
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=NUM_CODECS)
    x = torch.randn(8, 6)
    codec = torch.tensor(codec_one_hot_batch(["x264"] * 4 + ["libsvtav1"] * 4))
    y = torch.randn(8) * 20 + 50  # realistic MOS range
    loss = m._step((x, codec, y), "train")
    assert torch.isfinite(loss)
    loss.backward()
    # First linear layer's weight column slice for the codec one-hot
    # input should receive non-zero gradient — confirms the codec
    # input is actually wired into the graph.
    first_linear = next(p for p in m.net if isinstance(p, torch.nn.Linear))
    grads = first_linear.weight.grad
    assert grads is not None
    codec_columns = grads[:, 6:]  # last NUM_CODECS columns
    assert codec_columns.abs().sum() > 0.0


def test_fr_regressor_v1_batch_unchanged_with_num_codecs_zero() -> None:
    """Back-compat: existing 2-tuple (x, y) batches still work."""
    from vmaf_train.models import FRRegressor

    m = FRRegressor(in_features=6, num_codecs=0)
    x = torch.randn(4, 6)
    y = torch.randn(4) * 20 + 50
    loss = m._step((x, y), "train")
    assert torch.isfinite(loss)
