# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``aiutils.mini_corpus``: generated, deterministic, loader-compatible."""

from __future__ import annotations

from pathlib import Path

import pytest

from ai.data.netflix_loader import iter_pairs
from aiutils.mini_corpus import (
    CLIP_FRAMES,
    FRAME_BYTES,
    SOURCES,
    MiniCorpusError,
    generate_mini_corpus,
)

REPO = Path(__file__).resolve().parents[2]
REF = REPO / "testdata" / "ref_576x324_48f.yuv"
DIS = REPO / "testdata" / "dis_576x324_48f.yuv"


def test_layout_matches_the_netflix_loader(tmp_path: Path) -> None:
    corpus = generate_mini_corpus(tmp_path / "c", ref_yuv=REF, dis_yuv=DIS)
    assert corpus.pairs == len(SOURCES) * 3
    pairs = list(iter_pairs(corpus.root, assume_dims=(576, 324)))
    assert len(pairs) == corpus.pairs
    assert {p.source for p in pairs} == set(SOURCES)
    for p in pairs:
        assert p.dis_path.stat().st_size == CLIP_FRAMES * FRAME_BYTES
        assert p.ref_path.stat().st_size == CLIP_FRAMES * FRAME_BYTES


def test_same_seed_gives_byte_identical_corpora(tmp_path: Path) -> None:
    a = generate_mini_corpus(tmp_path / "a", ref_yuv=REF, dis_yuv=DIS, seed=3)
    b = generate_mini_corpus(tmp_path / "b", ref_yuv=REF, dis_yuv=DIS, seed=3)
    c = generate_mini_corpus(tmp_path / "c", ref_yuv=REF, dis_yuv=DIS, seed=4)
    assert a.sha256 == b.sha256
    assert a.sha256 != c.sha256  # the seed reaches the noise distortion


def test_distortions_differ_from_the_reference(tmp_path: Path) -> None:
    corpus = generate_mini_corpus(tmp_path / "c", ref_yuv=REF, dis_yuv=DIS)
    ref = (corpus.root / "ref" / "SrcA_24fps.yuv").read_bytes()
    for name in ("SrcA_20_324_500.yuv", "SrcA_10_324_250.yuv", "SrcA_30_324_1000.yuv"):
        assert (corpus.root / "dis" / name).read_bytes() != ref, name


def test_missing_input_yuv_is_named(tmp_path: Path) -> None:
    with pytest.raises(MiniCorpusError, match="input yuv missing"):
        generate_mini_corpus(tmp_path / "c", ref_yuv=tmp_path / "nope.yuv", dis_yuv=DIS)


def test_short_input_yuv_is_named_with_the_needed_size(tmp_path: Path) -> None:
    short = tmp_path / "short.yuv"
    short.write_bytes(REF.read_bytes()[: FRAME_BYTES * 5])
    with pytest.raises(MiniCorpusError, match=r"need \d+"):
        generate_mini_corpus(tmp_path / "c", ref_yuv=short, dis_yuv=DIS)
