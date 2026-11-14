# Copyright 2026 luoye-cpu
# SPDX-License-Identifier: EUPL-1.2
"""Unit tests for the near-duplicate spot check (ADR-3372, issue #3343).

``_verify_near_dup_spot`` must compare like with like: both sides of the
1.5-point delta come from the same one-second window at the entry's own
sample offset — the query source scored against the cached encode, and
the cached source scored against that same encode over the same window.
The cached trial's whole-clip aggregate must not enter the comparison,
and a cached entry without a readable source path, without a stored
source hash, or whose file no longer hashes to that hash fails closed
instead of comparing against a different or replaced baseline.
"""

from __future__ import annotations

import unittest.mock
from pathlib import Path
from types import SimpleNamespace

import pytest

from vmaftune.cache import CachedResult
from vmaftune.corpus import (
    NEAR_DUP_SPOT_MAX_DELTA,
    CorpusJob,
    CorpusOptions,
    _Sweep,
    _verify_near_dup_spot,
)
from vmaftune.per_shot import ShotMetadata
from vmaftune.score import ScoreResult


def _sweep(source: Path, *, frame_skip_ref: int = 0, frame_cnt: int = 24) -> _Sweep:
    """A minimal sweep state for the spot check (only job/opts are read)."""
    return _Sweep(
        job=CorpusJob(
            source=source,
            width=64,
            height=64,
            pix_fmt="yuv420p",
            framerate=24.0,
            duration_s=2.0,
            cells=(("medium", 23),),
        ),
        opts=CorpusOptions(),
        adapter=None,
        src_hash="a" * 64,
        tune_cache=None,
        clip_seconds=2.0,
        start_s=0.0,
        frame_skip_ref=frame_skip_ref,
        frame_cnt=frame_cnt,
        clip_mode="full",
        shot_meta=ShotMetadata(count=0, avg_duration_sec=0.0, duration_std_sec=0.0),
        hdr_info=None,
        hdr_forced=False,
        hdr_extra_params=(),
        score_model_warned=[False],
        decoded_reference=source,
        ref_decode_rc=0,
    )


def _source_digest(path: Path) -> str:
    from vmaftune.corpus import _sha256_file

    return _sha256_file(path)


def _cached(artifact: Path, src: Path | None, whole_clip_score: float) -> CachedResult:
    """A cached trial whose aggregate score covers its whole clip.

    The row mirrors a real one: ``src`` names the cached source and
    ``src_sha256`` its content hash.
    """
    row = None
    if src is not None:
        row = {"src": str(src)}
        if src.exists():
            row["src_sha256"] = _source_digest(src)
    return CachedResult(
        encode_size_bytes=1024,
        encode_time_ms=1.0,
        encoder_version="enc",
        ffmpeg_version="ff",
        vmaf_score=whole_clip_score,
        vmaf_model="vmaf_v0.6.1",
        score_time_ms=1.0,
        vmaf_binary_version="3",
        artifact_path=artifact,
        row=row,
    )


def _window_scores(calls: list[SimpleNamespace], scores: dict[Path, float]):
    """A ``run_score`` stand-in returning a per-reference window score."""

    def _run(req, *, vmaf_bin="vmaf", backend=None, **kw):
        calls.append(req)
        return ScoreResult(
            request=req,
            vmaf_score=scores[req.reference],
            score_time_ms=1.0,
            vmaf_binary_version="3",
            exit_status=0,
            stderr_tail="",
        )

    return _run


def _assert_same_window(calls: list) -> None:
    """Both spot scores must come from the same one-second window of the encode."""
    assert len(calls) == 2
    first, second = calls
    assert first.frame_skip_ref == 0 and second.frame_skip_ref == 0
    assert first.frame_cnt == 24 and second.frame_cnt == 24
    assert first.duration_s == 1.0 and second.duration_s == 1.0
    for field in (
        "distorted",
        "width",
        "height",
        "pix_fmt",
        "frame_skip_ref",
        "frame_cnt",
        "duration_s",
    ):
        assert getattr(first, field) == getattr(second, field), field


def test_exact_duplicate_whose_first_second_differs_from_mean_passes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """An exact duplicate passes even when its first second ≠ its mean (issue #3343).

    The whole-clip aggregate (90.0) and the first-second window score
    (85.0) differ by more than the 1.5-point ceiling, so the old
    whole-clip comparison refused an exact duplicate; the same-window
    comparison sees delta 0.
    """
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, source, whole_clip_score=90.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is True
    _assert_same_window(calls)
    # Both sides scored the cached encode over the same window.
    assert calls[0].distorted == artifact and calls[1].distorted == artifact


def test_different_clip_over_the_window_refuses(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A different clip whose window score drifts past the ceiling refuses."""
    query = tmp_path / "query.yuv"
    query.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, cached_src, whole_clip_score=82.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {query: 85.0, cached_src: 80.0}),
    ):
        assert _verify_near_dup_spot(_sweep(query), cached) is False
    _assert_same_window(calls)
    # The second score is the cached source against the cached encode.
    assert calls[1].reference == cached_src


def test_delta_exactly_at_the_ceiling_passes(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A window delta of exactly NEAR_DUP_SPOT_MAX_DELTA accepts (<=)."""
    assert NEAR_DUP_SPOT_MAX_DELTA == 1.5
    query = tmp_path / "query.yuv"
    query.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, cached_src, whole_clip_score=95.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {query: 85.0, cached_src: 83.5}),
    ):
        assert _verify_near_dup_spot(_sweep(query), cached) is True
    _assert_same_window(calls)


def test_cached_entry_without_a_source_row_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """No stored source path: fail closed instead of a whole-clip comparison."""
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, None, whole_clip_score=85.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is False
    assert calls == []


def test_cached_source_gone_fails_closed(tmp_path: Path, monkeypatch: pytest.MonkeyPatch) -> None:
    """A cached source path that no longer exists fails closed (issue #3343 fix)."""
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, tmp_path / "gone.yuv", whole_clip_score=85.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is False
    assert calls == []


def test_missing_cached_artifact_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A cached encode that is gone fails closed before any scoring."""
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    cached = _cached(tmp_path / "gone.mkv", cached_src, whole_clip_score=85.0)
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is False
    assert calls == []


def test_replaced_cached_source_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A cached source whose bytes no longer hash to the stored hash refuses.

    Without this check the spot check would score the replacement against
    the old encode: a replacement that happens to be a near-duplicate of
    the query would then pass as a zero delta (review on #3444).
    """
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, cached_src, whole_clip_score=85.0)
    cached_src.write_bytes(b"\xff" * 16)  # replaced after the encode was cached
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is False
    assert calls == []


def test_cached_source_without_stored_hash_fails_closed(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A row without a stored source hash cannot prove the baseline: refuse."""
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, cached_src, whole_clip_score=85.0)
    cached.row["src_sha256"] = ""
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0}),
    ):
        assert _verify_near_dup_spot(_sweep(source), cached) is False
    assert calls == []


def test_nonzero_sample_window_is_scored_at_its_offset(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """A sample-mode entry is verified at its own window, not at frame 0.

    The near-duplicate index only matches entries built with the same
    cell params, so the cached encode covers the window this sweep
    resolved: both spot scores skip to ``frame_skip_ref`` and cap at one
    second of the window (review on #3444).
    """
    source = tmp_path / "query.yuv"
    source.write_bytes(b"\x00" * 16)
    cached_src = tmp_path / "cached.yuv"
    cached_src.write_bytes(b"\x00" * 16)
    artifact = tmp_path / "enc.mkv"
    artifact.write_bytes(b"\x00" * 4)
    cached = _cached(artifact, cached_src, whole_clip_score=85.0)
    sweep = _sweep(source, frame_skip_ref=720, frame_cnt=120)  # 5 s clip from 30 s
    calls: list = []
    with unittest.mock.patch(
        "vmaftune.score.run_score",
        _window_scores(calls, {source: 85.0, cached_src: 85.0}),
    ):
        assert _verify_near_dup_spot(sweep, cached) is True
    assert len(calls) == 2
    for req in calls:
        assert req.frame_skip_ref == 720
        assert req.frame_cnt == 24  # one second of the 24 fps window
        assert req.duration_s == 1.0
