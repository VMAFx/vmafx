# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for :mod:`ai.data.netflix_loader`."""

from __future__ import annotations

import json
from pathlib import Path

import pytest

from ai.data import netflix_loader


def test_iter_pairs_pairs_dis_with_ref(mock_corpus: Path) -> None:
    pairs = list(netflix_loader.iter_pairs(mock_corpus, assume_dims=(16, 16)))
    assert len(pairs) == 4
    sources = {p.source for p in pairs}
    assert sources == {"AlphaSrc", "BetaSrc"}


def test_iter_pairs_extracts_ladder_metadata(mock_corpus: Path) -> None:
    pairs = list(netflix_loader.iter_pairs(mock_corpus, assume_dims=(16, 16)))
    by_name = {p.dis_path.name: p for p in pairs}
    bb = by_name["AlphaSrc_20_288_375.yuv"]
    assert bb.source == "AlphaSrc"
    assert bb.quality == 20
    assert bb.encode_height == 288
    assert bb.bitrate_kbps == 375
    assert bb.fps == 25  # parsed from AlphaSrc_25fps.yuv

    big = by_name["BetaSrc_85_1080_3800.yuv"]
    assert big.source == "BetaSrc"
    assert big.quality == 85
    assert big.encode_height == 1080
    assert big.bitrate_kbps == 3800
    assert big.fps == 30


def test_iter_pairs_filters_by_source(mock_corpus: Path) -> None:
    pairs = list(
        netflix_loader.iter_pairs(mock_corpus, sources=("AlphaSrc",), assume_dims=(16, 16))
    )
    assert len(pairs) == 2
    assert all(p.source == "AlphaSrc" for p in pairs)


def test_iter_pairs_max_pairs(mock_corpus: Path) -> None:
    pairs = list(netflix_loader.iter_pairs(mock_corpus, max_pairs=2, assume_dims=(16, 16)))
    assert len(pairs) == 2


def test_list_sources(mock_corpus: Path) -> None:
    assert netflix_loader.list_sources(mock_corpus) == ["AlphaSrc", "BetaSrc"]


def test_iter_pairs_skips_orphan_dis(mock_corpus: Path, tmp_path: Path) -> None:
    # Make a copy where one ref is missing.
    new_root = tmp_path / "orphan"
    (new_root / "ref").mkdir(parents=True)
    (new_root / "dis").mkdir()
    # Copy only one ref.
    (new_root / "ref" / "AlphaSrc_25fps.yuv").write_bytes(
        (mock_corpus / "ref" / "AlphaSrc_25fps.yuv").read_bytes()
    )
    for src in (mock_corpus / "dis").iterdir():
        (new_root / "dis" / src.name).write_bytes(src.read_bytes())
    pairs = list(netflix_loader.iter_pairs(new_root, assume_dims=(16, 16)))
    assert all(p.source == "AlphaSrc" for p in pairs)
    assert len(pairs) == 2


def test_probe_yuv_dims_falls_through_when_size_doesnt_match(mock_corpus: Path) -> None:
    # 16x16 synth doesn't match 1920x1080; ffprobe is unlikely to know
    # raw YUVs without flags, so this should raise unless ffprobe somehow
    # returns dims. The default path returns (16, 16) only via ffprobe;
    # otherwise raise. Either outcome is acceptable — the contract is
    # "no silent guess". Verify by confirming we either raise OR return
    # something not equal to the 1920x1080 default.
    p = next(iter((mock_corpus / "dis").iterdir()))
    try:
        w, h = netflix_loader.probe_yuv_dims(p)
    except RuntimeError:
        # Expected when ffprobe is absent or can't infer raw YUV dims.
        return
    assert (w, h) != (
        netflix_loader.DEFAULT_W,
        netflix_loader.DEFAULT_H,
    ), "Synth 16x16 must not be reported as 1920x1080."


def test_load_or_compute_caches(mock_corpus: Path, tmp_path: Path, monkeypatch) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    pair = next(iter(netflix_loader.iter_pairs(mock_corpus, assume_dims=(16, 16))))
    calls = {"n": 0}

    def compute(_p):
        calls["n"] += 1
        return {"hello": "world"}

    out1 = netflix_loader.load_or_compute(pair, compute)
    out2 = netflix_loader.load_or_compute(pair, compute)
    assert out1 == {"hello": "world"}
    assert out2 == {"hello": "world"}
    assert calls["n"] == 1, "compute_fn should run exactly once thanks to cache"

    # Confirm the JSON file landed where we expect.
    cache_file = netflix_loader.cache_path_for(pair)
    assert cache_file.is_file()
    assert json.loads(cache_file.read_text()) == {"hello": "world"}


def test_load_or_compute_recovers_from_corrupt_cache(
    mock_corpus: Path, tmp_path: Path, monkeypatch
) -> None:
    monkeypatch.setenv("VMAF_TINY_AI_CACHE", str(tmp_path / "cache"))
    pair = next(iter(netflix_loader.iter_pairs(mock_corpus, assume_dims=(16, 16))))
    cache_file = netflix_loader.cache_path_for(pair)
    cache_file.parent.mkdir(parents=True, exist_ok=True)
    cache_file.write_text("{not valid json")

    def compute(_p):
        return {"hello": "world"}

    out = netflix_loader.load_or_compute(pair, compute)
    assert out == {"hello": "world"}


def test_iter_pairs_missing_root_raises(tmp_path: Path) -> None:
    with pytest.raises(FileNotFoundError):
        list(netflix_loader.iter_pairs(tmp_path / "does-not-exist"))
