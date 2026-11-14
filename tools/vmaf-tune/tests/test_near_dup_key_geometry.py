# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Issue 3448 is unreachable while params_key includes the query geometry.

Cross-resolution reuse would require scoring at the cached row's geometry first.
"""

from __future__ import annotations

import hashlib
from dataclasses import replace
from pathlib import Path
from types import SimpleNamespace

import pytest

from vmaftune.cache import CachedResult, TuneCache
from vmaftune.corpus import CorpusJob, CorpusOptions, _cell_params_key
from vmaftune.fingerprint import VideoFingerprint, match

_GUARD_MESSAGE = (
    "Issue 3448: the spot check would score the cached encode at the query geometry; "
    "cross-resolution reuse must score at the cached row's geometry first"
)


def _sweep(**changes):
    job = CorpusJob(
        source=Path("source.yuv"),
        width=640,
        height=360,
        src_width=None,
        src_height=None,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    return SimpleNamespace(
        job=replace(job, **changes),
        opts=CorpusOptions(),
        adapter=SimpleNamespace(adapter_version="1"),
        ffmpeg_version="test",
        clip_seconds=0.0,
        start_s=0.0,
        hdr_extra_params=(),
        score_model="vmaf_v0.6.1",
    )


def _fingerprint(width, height):
    frames = [(i / 20.0, hashlib.sha256(bytes([i])).digest(), 100) for i in range(40)]
    return VideoFingerprint(
        duration_s=2.0, width=width, height=height, dar="16:9", par="1:1", frames=frames
    )


@pytest.fixture
def cached_baseline(tmp_path):
    cache = TuneCache(tmp_path / "cache")
    artifact = tmp_path / "encode.bin"
    artifact.write_bytes(b"x")
    result = CachedResult(1, 1.0, "test", "test", 95.0, "vmaf_v0.6.1", 1.0, "test", artifact)
    fingerprint = _fingerprint(640, 360)
    key = _cell_params_key(_sweep(), "medium", 23)
    cache.put(
        "baseline",
        result,
        artifact,
        fingerprint=fingerprint,
        params_key=key,
        src_sha256="a" * 64,
    )
    return cache, fingerprint, key


def test_same_geometry_key_allows_near_duplicate(cached_baseline):
    cache, _, cached_key = cached_baseline
    query_key = _cell_params_key(_sweep(), "medium", 23)
    assert query_key == cached_key, _GUARD_MESSAGE
    assert cache.get_near_duplicate(_fingerprint(640, 360), query_key) is not None, _GUARD_MESSAGE


def test_other_resolution_key_refuses_matching_fingerprint(cached_baseline):
    cache, cached_fingerprint, cached_key = cached_baseline
    query_fingerprint = _fingerprint(320, 180)
    query_key = _cell_params_key(_sweep(width=320, height=180), "medium", 23)
    assert match(query_fingerprint, cached_fingerprint).accepted is True, _GUARD_MESSAGE
    assert query_key != cached_key, _GUARD_MESSAGE
    assert cache.get_near_duplicate(query_fingerprint, query_key) is None, _GUARD_MESSAGE


@pytest.mark.parametrize(
    "changes",
    [
        pytest.param({"width": 642}, id="width"),
        pytest.param({"height": 362}, id="height"),
        pytest.param({"src_width": 1280, "src_height": 720}, id="src_width-src_height"),
        pytest.param({"pix_fmt": "yuv420p10le"}, id="pix_fmt"),
        pytest.param({"framerate": 25.0}, id="framerate"),
    ],
)
def test_geometry_field_change_changes_key(changes):
    baseline_key = _cell_params_key(_sweep(), "medium", 23)
    changed_key = _cell_params_key(_sweep(**changes), "medium", 23)
    assert changed_key != baseline_key, _GUARD_MESSAGE
