# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Near-duplicate spot checks decode cached containers before scoring."""

from __future__ import annotations

import dataclasses
import shutil
from types import SimpleNamespace

import pytest
from _vmaf_cli import run_absolute_tool

from vmaftune import corpus, score
from vmaftune.cache import CachedResult
from vmaftune.corpus import CorpusJob, CorpusOptions


@pytest.fixture
def spot(tmp_path):
    cache_dir = tmp_path / "cache"
    cache_dir.mkdir()
    artifact = cache_dir / "blob.bin"
    artifact.write_bytes(b"container")
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
    )
    cached = CachedResult(9, 1.0, "test", "test", 95.0, "test", 1.0, "test", artifact)
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
        assert req.reference == sweep.decoded_reference
        assert req.reference != sweep.job.source
        assert req.distorted.suffix == ".yuv"
        assert req.distorted == decoded_paths[0]
        assert req.distorted.exists()
        scored_paths.append(req.distorted)
        if error is not None:
            raise error("scoring failed")
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(corpus, "_maybe_decode_distorted", decode)
    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is (error is None)
    assert len(scored_paths) == 1
    assert not decoded_paths[0].parent.exists()
    assert list(cached.artifact_path.parent.iterdir()) == [cached.artifact_path]


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


def _generate_cached_encode(sweep, cached, ffmpeg):
    common = [ffmpeg, "-y", "-hide_banner", "-loglevel", "error"]
    run_absolute_tool(
        [
            *common,
            "-f",
            "lavfi",
            "-i",
            "testsrc2=size=320x240:rate=24:duration=1",
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
            "-i",
            str(sweep.decoded_reference),
            "-c:v",
            "libx264",
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
    scored_paths = []

    def run_score(req, **kwargs):
        assert req.reference == sweep.decoded_reference
        assert sweep.job.source.suffix == ".mkv"
        assert sweep.job.source.exists()
        assert req.distorted.suffix == ".yuv"
        assert req.distorted.parent != cached.artifact_path.parent
        size = req.distorted.stat().st_size
        frame_bytes = 320 * 240 * 3 // 2
        assert size > 0 and size % frame_bytes == 0
        assert size == frame_bytes * 24
        scored_paths.append(req.distorted)
        return SimpleNamespace(vmaf_score=cached.vmaf_score)

    monkeypatch.setattr(score, "run_score", run_score)
    assert corpus._verify_near_dup_spot(sweep, cached) is True
    assert len(scored_paths) == 1
    assert not scored_paths[0].parent.exists()
    assert list(cached.artifact_path.parent.iterdir()) == [cached.artifact_path]
