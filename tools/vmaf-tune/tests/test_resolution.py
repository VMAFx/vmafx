# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Resolution-aware model selection + CRF offset tests.

Verifies the decision rule documented in
``docs/adr/0289-vmaf-tune-resolution-aware.md`` and
``docs/research/0064-vmaf-tune-resolution-aware.md``:

- height >= 2160 → 4K model
- else → 1080p model (canonical fallback for 720p / SD / sub-SD too)
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install (mirror test_corpus.py).
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.corpus import CorpusJob, CorpusOptions, iter_rows
from vmaftune.resolution import (
    MODEL_4K,
    MODEL_1080P,
    crf_offset_for_resolution,
    select_vmaf_model,
    select_vmaf_model_version,
)

# -----------------------------------------------------------------------------
# select_vmaf_model_version — the version-string return.
# -----------------------------------------------------------------------------


def test_1080p_picks_1080p_model():
    assert select_vmaf_model_version(1920, 1080) == MODEL_1080P
    assert MODEL_1080P == "vmaf_v0.6.1"


def test_2160p_picks_4k_model():
    assert select_vmaf_model_version(3840, 2160) == MODEL_4K
    assert MODEL_4K == "vmaf_4k_v0.6.1"


def test_720p_falls_back_to_1080p_model():
    # The fork has no 720p model — 1080p is the canonical fallback.
    assert select_vmaf_model_version(1280, 720) == MODEL_1080P


def test_sd_falls_back_to_1080p_model():
    # 576x324 (Netflix golden) — sub-720p still routes to 1080p.
    assert select_vmaf_model_version(576, 324) == MODEL_1080P


def test_8k_routes_to_4k_model():
    # No 8K-specific model in the fork; 4K is the highest available.
    assert select_vmaf_model_version(7680, 4320) == MODEL_4K


def test_just_below_2160_is_still_1080p():
    # Boundary: 2159 stays on 1080p; 2160 flips to 4K.
    assert select_vmaf_model_version(3840, 2159) == MODEL_1080P
    assert select_vmaf_model_version(3840, 2160) == MODEL_4K


def test_invalid_resolution_raises():
    with pytest.raises(ValueError):
        select_vmaf_model_version(0, 1080)
    with pytest.raises(ValueError):
        select_vmaf_model_version(1920, -1)


# -----------------------------------------------------------------------------
# select_vmaf_model — the Path-resolving wrapper.
# -----------------------------------------------------------------------------


def test_select_vmaf_model_returns_existing_json():
    # Hard-locates the in-tree model/ dir; the JSON files must exist.
    p = select_vmaf_model(1920, 1080)
    assert p.name == "vmaf_v0.6.1.json"
    assert p.exists(), f"expected in-tree model file at {p}"

    p4k = select_vmaf_model(3840, 2160)
    assert p4k.name == "vmaf_4k_v0.6.1.json"
    assert p4k.exists(), f"expected in-tree 4K model file at {p4k}"


# -----------------------------------------------------------------------------
# crf_offset_for_resolution — sensible defaults.
# -----------------------------------------------------------------------------


def test_crf_offset_1080p_baseline_is_zero():
    assert crf_offset_for_resolution(1920, 1080) == 0


def test_crf_offset_4k_is_negative():
    # 4K under-shoots at parity CRF — needs a small negative offset.
    val = crf_offset_for_resolution(3840, 2160)
    assert val < 0
    assert -4 <= val <= -1, f"unexpected 4K offset {val}"


def test_crf_offset_720p_is_positive():
    # 720p over-shoots at parity CRF — small positive offset.
    val = crf_offset_for_resolution(1280, 720)
    assert val > 0
    assert 1 <= val <= 4, f"unexpected 720p offset {val}"


def test_crf_offset_sd_is_more_positive_than_720p():
    sd = crf_offset_for_resolution(640, 360)
    hd = crf_offset_for_resolution(1280, 720)
    assert sd >= hd


def test_crf_offset_8k_clamps_to_4k_offset():
    # No 8K-specific tuning yet — same offset as 4K.
    assert crf_offset_for_resolution(7680, 4320) == crf_offset_for_resolution(3840, 2160)


def test_crf_offset_invalid_resolution_raises():
    with pytest.raises(ValueError):
        crf_offset_for_resolution(-1, 1080)


# -----------------------------------------------------------------------------
# Integration — corpus.iter_rows respects the resolution_aware flag.
# -----------------------------------------------------------------------------


class _FakeCompleted:
    def __init__(self, returncode: int, stdout: str = "", stderr: str = ""):
        self.returncode = returncode
        self.stdout = stdout
        self.stderr = stderr


def _fake_encode(cmd, capture_output, text, check):
    out_path = Path(cmd[-1])
    out_path.write_bytes(b"\x00" * 4096)
    return _FakeCompleted(returncode=0, stderr="ffmpeg version 6.1.1\nx264 - core 164 r3107\n")


def _fake_score(cmd, capture_output, text, check):
    out_idx = cmd.index("--output") + 1
    out_path = Path(cmd[out_idx])
    out_path.parent.mkdir(parents=True, exist_ok=True)
    out_path.write_text(json.dumps({"pooled_metrics": {"vmaf": {"mean": 92.5}}}))
    return _FakeCompleted(returncode=0, stderr="VMAF version: 3.0.0-lusoris\n")


def _make_yuv(path: Path) -> Path:
    path.write_bytes(b"\x80" * 1024)
    return path


def test_iter_rows_resolution_aware_4k_picks_4k_model(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref_4k.yuv")
    job = CorpusJob(
        source=src,
        width=3840,
        height=2160,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
        # vmaf_model is the wrong one on purpose — resolution-aware must override.
        vmaf_model="vmaf_v0.6.1",
        resolution_aware=True,
    )
    rows = list(iter_rows(job, opts, encode_runner=_fake_encode, score_runner=_fake_score))
    assert len(rows) == 1
    assert rows[0]["vmaf_model"] == MODEL_4K


def test_iter_rows_resolution_aware_off_keeps_explicit_model(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref_4k.yuv")
    job = CorpusJob(
        source=src,
        width=3840,
        height=2160,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
        vmaf_model="vmaf_v0.6.1",
        resolution_aware=False,
    )
    rows = list(iter_rows(job, opts, encode_runner=_fake_encode, score_runner=_fake_score))
    assert len(rows) == 1
    # With auto-selection off, the explicit opt wins even on 4K input.
    assert rows[0]["vmaf_model"] == "vmaf_v0.6.1"


def test_iter_rows_resolution_aware_1080p_keeps_1080p_model(tmp_path: Path):
    src = _make_yuv(tmp_path / "ref_1080p.yuv")
    job = CorpusJob(
        source=src,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=2.0,
        cells=(("medium", 23),),
    )
    opts = CorpusOptions(
        output=tmp_path / "corpus.jsonl",
        encode_dir=tmp_path / "encodes",
        src_sha256=False,
        resolution_aware=True,
    )
    rows = list(iter_rows(job, opts, encode_runner=_fake_encode, score_runner=_fake_score))
    assert len(rows) == 1
    assert rows[0]["vmaf_model"] == MODEL_1080P
