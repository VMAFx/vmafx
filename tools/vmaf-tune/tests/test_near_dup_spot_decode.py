# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Near-duplicate spot checks decode cached containers before scoring.

Adapted for the same-window two-leg comparison of PR #3444 (issue
#3343): the query leg scores ``sweep.decoded_reference`` against the
decoded cached encode, and the cached-source leg — added by this PR —
scores the row's source against that same encode over the same window,
so a scoring run covers both legs. Geometry stays the job's: the
near-duplicate index matches on the cell params, which carry width,
height, pixel format and frame rate, so a cached entry at another
geometry never reaches the spot check (issue #3448, closed as not
planned).
"""

from __future__ import annotations

import dataclasses
import hashlib
import logging
import shutil
from pathlib import Path
from types import SimpleNamespace

import pytest
from _vmaf_cli import run_absolute_tool, vmaf_under_test

from vmaftune import corpus, score
from vmaftune.cache import CachedResult
from vmaftune.corpus import CorpusJob, CorpusOptions
from vmaftune.score import DEFAULT_MODEL


def _row_for(path: Path) -> dict:
    """A cache row whose ``src`` column names ``path`` with its content hash."""
    return {
        "src": str(path),
        "width": 320,
        "height": 240,
        "pix_fmt": "yuv420p",
        "framerate": 24.0,
        "src_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
    }


@pytest.fixture
def spot(tmp_path):
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir()
    artifact = cache_dir / "blob.bin"
    artifact.write_bytes(b"container")
    cached_source = tmp_path / "cached-source.yuv"
    cached_source.write_bytes(b"cached source")
    sweep = SimpleNamespace(
        job=CorpusJob(
            source=tmp_path / "source.mkv",
            width=320,
            height=240,
            pix_fmt="yuv420p",
            framerate=24.0,
            duration_s=1.0,
            cells=(("medium", 23),),
        ),
        opts=CorpusOptions(),
        decoded_reference=tmp_path / "reference.yuv",
        ref_decode_rc=0,
        frame_skip_ref=0,
        start_s=0.0,
        score_model=DEFAULT_MODEL,
    )
    cached = CachedResult(
        9,
        1.0,
        "test",
        "test",
        95.0,
        "test",
        1.0,
        "test",
        artifact,
        row=_row_for(cached_source),
    )
    return sweep, cached


@pytest.mark.parametrize("error", [None, OSError, corpus.subprocess.SubprocessError, ValueError])
def test_spot_scores_decoded_yuv_and_cleans_up(spot, monkeypatch, error):
    sweep, cached = spot
    decoded_paths = []
    scored_paths = []

    def decode(req, *, encode_dir, ffmpeg_bin, runner):
        assert req.duration_s == 1.0
        assert ffmpeg_bin == sweep.opts.ffmpeg_bin
        assert runner is corpus.subprocess.run
        assert encode_dir != cached.artifact_path.parent
        decoded = encode_dir / "blob.decoded.yuv"
        decoded.write_bytes(b"raw")
        decoded_paths.append(decoded)
        return dataclasses.replace(req, distorted=decoded)

    def run_score(req, **kwargs):
        assert req.distorted.suffix == ".yuv"
        assert req.distorted == decoded_paths[0]
        assert req.distorted.exists()
        scored_paths.append(req)
        if error is not None:
            raise error("scoring failed")
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(corpus, "_maybe_decode_distorted", decode)
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is (error is None)
    # Two legs: the query reference and the cached source, same window.
    assert len(scored_paths) == (2 if error is None else 1)
    assert scored_paths[0].reference == sweep.decoded_reference
    if error is None:
        assert scored_paths[1].reference == Path(cached.row["src"])
    assert not decoded_paths[0].parent.exists()
    assert list(cached.artifact_path.parent.iterdir()) == [cached.artifact_path]


def test_spot_forwards_sample_window_reference_skip(spot, monkeypatch):
    sweep, cached = spot
    sweep.frame_skip_ref = 48
    sweep.start_s = 2.0

    def run_score(req, **kwargs):
        assert req.frame_skip_ref == 48
        assert req.frame_cnt == 24
        assert req.duration_s == 1.0
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(
        corpus,
        "_maybe_decode_distorted",
        lambda req, **kwargs: dataclasses.replace(req, distorted=sweep.decoded_reference),
    )
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is True


def test_spot_scores_with_sweep_model(spot, monkeypatch):
    sweep, cached = spot
    sweep.score_model = "vmaf_4k_v0.6.1"

    def run_score(req, **kwargs):
        assert req.model == sweep.score_model
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(
        corpus,
        "_maybe_decode_distorted",
        lambda req, **kwargs: dataclasses.replace(req, distorted=sweep.decoded_reference),
    )
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is True


def test_spot_refuses_failed_decode_without_scoring(spot, monkeypatch, caplog):
    sweep, cached = spot
    decode_dirs = []
    scored_paths = []

    def decode(req, *, encode_dir, **kwargs):
        (encode_dir / "partial.yuv").write_bytes(b"partial")
        decode_dirs.append(encode_dir)
        return req

    def run_score(req, **kwargs):
        scored_paths.append(req.distorted)
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(corpus, "_maybe_decode_distorted", decode)
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is False
    assert scored_paths == []
    assert len(caplog.records) == 1
    assert str(cached.artifact_path) in caplog.text
    assert not decode_dirs[0].exists()


def test_spot_refuses_failed_reference_decode(spot, monkeypatch, caplog):
    sweep, cached = spot
    sweep.ref_decode_rc = 1

    def unexpected_call(*args, **kwargs):
        pytest.fail("failed reference decode must stop before decoding or scoring")

    monkeypatch.setattr(corpus, "_maybe_decode_distorted", unexpected_call)
    monkeypatch.setattr(score, "run_score", unexpected_call)
    assert corpus._verify_near_dup_spot(sweep, cached) is False
    assert len(caplog.records) == 1
    assert str(sweep.job.source) in caplog.text


def test_spot_refuses_a_replaced_cached_source(spot, monkeypatch, caplog):
    """A cached source whose bytes no longer hash to the row refuses reuse."""
    sweep, cached = spot
    caplog.set_level(logging.INFO)
    cached.row["src_sha256"] = hashlib.sha256(b"original bytes").hexdigest()

    scored_paths = []

    def run_score(req, **kwargs):
        scored_paths.append(req.reference)
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(
        corpus,
        "_maybe_decode_distorted",
        lambda req, **kwargs: dataclasses.replace(req, distorted=sweep.decoded_reference),
    )
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is False
    assert scored_paths == []
    assert len(caplog.records) == 1


def _generate_cached_encode(sweep, cached, ffmpeg):
    common = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error"]
    run_absolute_tool(
        [
            *common,
            "-f",
            "lavfi",
            "-i",
            f"testsrc2=size=320x240:rate=24:duration={sweep.job.duration_s}",
            "-pix_fmt",
            "yuv420p",
            "-f",
            "rawvideo",
            str(sweep.decoded_reference),
        ],
        check=True,
        capture_output=True,
        timeout=20,
    )
    run_absolute_tool(
        [
            *common,
            "-f",
            "rawvideo",
            "-pix_fmt",
            "yuv420p",
            "-s",
            "320x240",
            "-r",
            "24",
            "-ss",
            str(sweep.start_s),
            "-t",
            "1",
            "-i",
            str(sweep.decoded_reference),
            "-c:v",
            "libx264",
            "-crf",
            "23",
            "-threads",
            "1",
            "-f",
            "matroska",
            str(cached.artifact_path),
        ],
        check=True,
        capture_output=True,
        timeout=20,
    )
    shutil.copyfile(cached.artifact_path, sweep.job.source)


def test_spot_decodes_real_cached_container(spot, monkeypatch):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        pytest.skip("ffmpeg not on PATH")
    sweep, cached = spot
    _generate_cached_encode(sweep, cached, ffmpeg)
    cached = dataclasses.replace(cached, row=_row_for(sweep.job.source))
    scored = []
    ref_sizes = []

    def run_score(req, **kwargs):
        assert sweep.job.source.suffix == ".mkv"
        assert sweep.job.source.exists()
        assert req.distorted.suffix == ".yuv"
        assert req.distorted.parent != cached.artifact_path.parent
        size = req.distorted.stat().st_size
        frame_bytes = 320 * 240 * 3 // 2
        assert size > 0 and size % frame_bytes == 0
        assert size == frame_bytes * 24
        # Both legs score a reference that exists while the check runs;
        # the temporary decode directory is gone by the time we assert.
        ref_sizes.append(req.reference.stat().st_size)
        scored.append(req)
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is True
    # Two legs: the query reference (the sweep's pre-decoded raw) and the
    # cached source decoded out of its container into the temp directory.
    assert len(scored) == 2
    assert scored[0].reference == sweep.decoded_reference
    assert scored[1].reference != sweep.job.source
    assert scored[1].reference.suffix == ".yuv"
    frame_bytes = 320 * 240 * 3 // 2
    assert ref_sizes == [frame_bytes * 24, frame_bytes * 24]
    assert not scored[0].distorted.parent.exists()
    assert list(cached.artifact_path.parent.iterdir()) == [cached.artifact_path]


def _real_window_sweep(spot, ffmpeg):
    """A 3 s sweep whose cached encode covers source frames 24-47."""
    sweep, cached = spot
    sweep.opts = dataclasses.replace(sweep.opts, ffmpeg_bin=ffmpeg, vmaf_bin=str(vmaf_under_test()))
    sweep.job = dataclasses.replace(sweep.job, duration_s=3.0)
    sweep.frame_skip_ref = 24
    sweep.start_s = 1.0
    _generate_cached_encode(sweep, cached, ffmpeg)
    return sweep, cached


def test_spot_scores_real_sample_window(spot, tmp_path):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        pytest.skip("ffmpeg not on PATH")
    sweep, cached = _real_window_sweep(spot, ffmpeg)
    req = score.ScoreRequest(
        reference=sweep.decoded_reference,
        distorted=cached.artifact_path,
        width=320,
        height=240,
        pix_fmt="yuv420p",
        frame_skip_ref=24,
        frame_cnt=24,
        duration_s=1.0,
    )
    decoded_req = corpus._maybe_decode_distorted(
        req, encode_dir=tmp_path, ffmpeg_bin=ffmpeg, runner=run_absolute_tool
    )
    assert decoded_req.distorted.suffix == ".yuv"
    result = score.run_score(decoded_req, vmaf_bin=sweep.opts.vmaf_bin, runner=run_absolute_tool)
    assert result.exit_status == 0
    # The encode covers source frames 24-47: scored against that window it is near-transparent.
    assert result.vmaf_score > 90.0
    cached = dataclasses.replace(cached, vmaf_score=result.vmaf_score)
    # The row names the sweep's pre-decoded 3 s reference: the same
    # content the cached encode was built from, so the cached-source leg
    # has frames at the sample offset. (Naming the 1 s encode copy — the
    # old fixture — left the leg with no frame at skip 24; review on
    # #3444.)
    cached = dataclasses.replace(cached, row=_row_for(sweep.decoded_reference))
    assert corpus._verify_near_dup_spot(sweep, cached) is True


def test_spot_refuses_a_different_real_cached_source(spot, tmp_path):
    ffmpeg = shutil.which("ffmpeg")
    if ffmpeg is None:
        pytest.skip("ffmpeg not on PATH")
    sweep, cached = _real_window_sweep(spot, ffmpeg)
    different = tmp_path / "different-source.mkv"
    run_absolute_tool(
        [
            ffmpeg,
            "-y",
            "-hide_banner",
            "-loglevel",
            "error",
            "-f",
            "lavfi",
            "-i",
            f"smptebars=size=320x240:rate=24:duration={sweep.job.duration_s}",
            "-pix_fmt",
            "yuv420p",
            "-c:v",
            "libx264",
            "-crf",
            "23",
            "-threads",
            "1",
            "-f",
            "matroska",
            str(different),
        ],
        check=True,
        capture_output=True,
        timeout=20,
    )
    cached = dataclasses.replace(cached, row=_row_for(different))
    assert corpus._verify_near_dup_spot(sweep, cached) is False
