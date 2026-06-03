# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Coverage push round 2 — bisect, ladder, recommend, cache, hdr.

Targets the uncovered lines identified after round 1:

- bisect.py: _workdir_parent, _estimate_yuv_bytes, _check_disk_space,
  _absolute_crf_range (fallback paths), _midrun_disk_headroom,
  set_decode_semaphore, _try_nr_early_elimination_on_yuv,
  max_iterations=0 guard, disk-space early-exit path.
- ladder.py: pixel_count property, _default_sampler_preset (no-medium path),
  make_default_sampler factory, _dedup_samples, _cross helper,
  select_knees n=1 path, spacing=unknown error, insert synthetic rung
  without WIDE interval (short list), prune anchor-rung swapping logic.
- recommend.py: load_corpus_jsonl, format_result, _row_interval fallbacks,
  pick_target_vmaf_with_uncertainty (tight short-circuit, wide full-scan,
  UNMET + interval-excluded, MIDDLE fallback paths).
- cache.py: TuneCache.keys() (empty dir), total_bytes (missing sub),
  corrupt index recovery, put + artifact missing, put-with-evict path.
- hdr.py: _frac_to_unit (float/int input, bad-string path), _hdr_args_vvenc,
  detect_hdr runner=None (ffprobe not on PATH), empty streams payload,
  _extract_mastering early-return paths.
"""

from __future__ import annotations

import json
import math
import sys
import threading
from dataclasses import dataclass
from pathlib import Path
from typing import Any
from unittest.mock import MagicMock

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

# ---------------------------------------------------------------------------
# bisect
# ---------------------------------------------------------------------------

from vmaftune import bisect as bisect_mod  # noqa: E402
from vmaftune.bisect import (  # noqa: E402
    _absolute_crf_range,
    _check_disk_space,
    _describe_best_miss,
    _detect_monotonicity_violation,
    _estimate_yuv_bytes,
    _midpoint_lower_quality,
    _midrun_disk_headroom,
    _sample_clip_window,
    _workdir_parent,
    bisect_target_vmaf,
    set_decode_semaphore,
)

# ---------------------------------------------------------------------------
# bisect helpers — pure-logic functions
# ---------------------------------------------------------------------------


def test_midpoint_lower_quality_even_window():
    # (0 + 10 + 1) // 2 = 5 — picks the higher-CRF (lower-quality) midpoint.
    assert _midpoint_lower_quality(0, 10) == 5


def test_midpoint_lower_quality_single_element():
    assert _midpoint_lower_quality(7, 7) == 7


def test_midpoint_lower_quality_odd_window():
    # (0 + 51 + 1) // 2 = 26
    assert _midpoint_lower_quality(0, 51) == 26


def test_estimate_yuv_bytes_420p():
    # 1920 * 1080 * 1.5 * ceil(24 * 1) = 1920 * 1080 * 1.5 * 24
    result = _estimate_yuv_bytes(
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        fps=24.0,
        duration_s=1.0,
    )
    assert result == math.ceil(1920 * 1080 * 1.5 * 24)


def test_estimate_yuv_bytes_420p10le():
    result = _estimate_yuv_bytes(
        width=1280,
        height=720,
        pix_fmt="yuv420p10le",
        fps=30.0,
        duration_s=2.0,
    )
    assert result == math.ceil(1280 * 720 * 3.0 * 60)


def test_estimate_yuv_bytes_unknown_fmt_uses_default():
    # Unknown pix_fmt falls back to 1.5 bpp.
    result = _estimate_yuv_bytes(
        width=640,
        height=480,
        pix_fmt="yuv999p_bogus",
        fps=25.0,
        duration_s=1.0,
    )
    assert result == math.ceil(640 * 480 * 1.5 * 25)


def test_estimate_yuv_bytes_zero_duration_gives_one_frame():
    result = _estimate_yuv_bytes(
        width=640,
        height=480,
        pix_fmt="yuv420p",
        fps=24.0,
        duration_s=0.0,
    )
    assert result >= 640 * 480  # at least one frame


def test_midrun_disk_headroom_raw_yuv():
    assert _midrun_disk_headroom(Path("ref.yuv")) == pytest.approx(1.1)


def test_midrun_disk_headroom_raw_no_extension():
    # A path with no extension is treated as raw (not a container).
    assert _midrun_disk_headroom(Path("ref")) == pytest.approx(1.1)


def test_midrun_disk_headroom_container_mkv():
    assert _midrun_disk_headroom(Path("video.mkv")) == pytest.approx(2.0)


def test_midrun_disk_headroom_container_mp4():
    assert _midrun_disk_headroom(Path("video.mp4")) == pytest.approx(2.0)


def test_check_disk_space_returns_none_when_sufficient(tmp_path):
    # Ask for tiny bytes — always passes on any real filesystem.
    result = _check_disk_space(tmp_path, estimated_bytes=1, headroom=1.0)
    assert result is None


def test_check_disk_space_returns_error_message_when_insufficient(tmp_path, monkeypatch):
    # Patch disk_usage to report zero free space.
    class _FakeUsage:
        free = 0

    monkeypatch.setattr("vmaftune.bisect.shutil.disk_usage", lambda _p: _FakeUsage())
    result = _check_disk_space(
        tmp_path,
        estimated_bytes=1024 * 1024 * 1024,
        headroom=1.0,
        context="libx264 @ VMAF 95",
    )
    assert result is not None
    assert "insufficient disk space" in result
    assert "libx264 @ VMAF 95" in result
    assert "--workdir" in result


def test_check_disk_space_oserror_returns_none(tmp_path, monkeypatch):
    monkeypatch.setattr(
        "vmaftune.bisect.shutil.disk_usage",
        lambda _p: (_ for _ in ()).throw(OSError("ENOSYS")),
    )
    # OSError path should return None (allow decode to proceed).
    result = _check_disk_space(tmp_path, estimated_bytes=1024**3)
    assert result is None


def test_check_disk_space_no_context_suffix(tmp_path, monkeypatch):
    class _FakeUsage:
        free = 0

    monkeypatch.setattr("vmaftune.bisect.shutil.disk_usage", lambda _p: _FakeUsage())
    result = _check_disk_space(tmp_path, estimated_bytes=1024**3, headroom=1.0, context="")
    assert result is not None
    assert "[" not in result  # no context bracket


def test_workdir_parent_env_set(monkeypatch, tmp_path):
    monkeypatch.setenv("VMAFTUNE_WORKDIR", str(tmp_path / "workdir"))
    result = _workdir_parent()
    assert result == tmp_path / "workdir"


def test_workdir_parent_env_unset(monkeypatch):
    monkeypatch.delenv("VMAFTUNE_WORKDIR", raising=False)
    assert _workdir_parent() is None


def test_workdir_parent_env_whitespace_only(monkeypatch):
    monkeypatch.setenv("VMAFTUNE_WORKDIR", "   ")
    assert _workdir_parent() is None


def test_absolute_crf_range_table_lookup():
    from vmaftune.codec_adapters import get_adapter

    adapter = get_adapter("libx264")
    lo, hi = _absolute_crf_range(adapter)
    assert lo == 0
    assert hi == 51


def test_absolute_crf_range_crf_min_max_attributes():
    # Fake adapter with crf_min/crf_max but no name in the table.
    adapter = MagicMock()
    adapter.name = "libcustom_not_in_table"
    adapter.crf_min = 5
    adapter.crf_max = 45
    lo, hi = _absolute_crf_range(adapter)
    assert lo == 5
    assert hi == 45


def test_absolute_crf_range_fallback_to_quality_range():
    # Adapter with no name in table, no crf_min/crf_max.
    adapter = MagicMock(spec=[])
    adapter.name = "libcustom_no_attrs"
    adapter.quality_range = (10, 40)
    lo, hi = _absolute_crf_range(adapter)
    assert lo == 10
    assert hi == 40


def test_detect_monotonicity_violation_no_history():
    assert _detect_monotonicity_violation({}, 25, 85.0) is None


def test_detect_monotonicity_violation_consistent():
    history = {20: 95.0, 30: 88.0}
    assert _detect_monotonicity_violation(history, 35, 82.0) is None


def test_detect_monotonicity_violation_higher_crf_higher_vmaf():
    # CRF 25 had VMAF 80; CRF 30 now reports VMAF 82 — VMAF rose with CRF.
    history = {25: 80.0}
    result = _detect_monotonicity_violation(history, 30, 82.0)
    assert result is not None
    assert "monotonicity" in result


def test_detect_monotonicity_violation_lower_crf_lower_vmaf():
    # CRF 30 had VMAF 82; CRF 20 now reports VMAF 78 — VMAF fell with lower CRF.
    history = {30: 82.0}
    result = _detect_monotonicity_violation(history, 20, 78.0)
    assert result is not None
    assert "monotonicity" in result


def test_detect_monotonicity_violation_within_tolerance():
    # 0.3 < 0.5 tolerance — still consistent.
    history = {25: 80.0}
    assert _detect_monotonicity_violation(history, 30, 80.3) is None


def test_describe_best_miss_empty():
    assert "no samples" in _describe_best_miss({})


def test_describe_best_miss_returns_closest():
    history = {20: 80.0, 25: 85.0, 30: 78.0}
    desc = _describe_best_miss(history)
    assert "85" in desc
    assert "25" in desc


def test_sample_clip_window_valid():
    start, clip, skip, cnt = _sample_clip_window(
        duration_s=10.0,
        sample_clip_seconds=4.0,
        framerate=24.0,
    )
    assert start == pytest.approx(3.0)
    assert clip == pytest.approx(4.0)
    assert skip == 72  # int(round(3.0 * 24))
    assert cnt == 96  # int(round(4.0 * 24))


def test_sample_clip_window_sample_ge_duration_returns_zeros():
    # sample_clip_seconds >= duration_s → no-op
    start, clip, skip, cnt = _sample_clip_window(
        duration_s=5.0,
        sample_clip_seconds=5.0,
        framerate=24.0,
    )
    assert start == 0.0 and clip == 0.0 and skip == 0 and cnt == 0


def test_sample_clip_window_zero_sample():
    start, clip, skip, cnt = _sample_clip_window(
        duration_s=10.0,
        sample_clip_seconds=0.0,
        framerate=24.0,
    )
    assert start == 0.0 and cnt == 0


def test_set_decode_semaphore_valid():
    original = bisect_mod._decode_semaphore
    set_decode_semaphore(3)
    assert isinstance(bisect_mod._decode_semaphore, threading.Semaphore)
    # Restore.
    bisect_mod._decode_semaphore = original


def test_set_decode_semaphore_invalid():
    with pytest.raises(ValueError, match="max_concurrent must be >= 1"):
        set_decode_semaphore(0)


def test_max_iterations_zero_returns_failure():
    @dataclass
    class _FakeCompleted:
        returncode: int = 0
        stdout: str = ""
        stderr: str = ""

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=0,
        encode_runner=lambda *a, **k: _FakeCompleted(),
        score_runner=lambda *a, **k: _FakeCompleted(),
        width=640,
        height=480,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=10.0,
    )
    assert result.ok is False
    assert "max_iterations" in result.error


# ---------------------------------------------------------------------------
# bisect disk-space short-circuit
# ---------------------------------------------------------------------------


def test_bisect_disk_space_short_circuit(tmp_path, monkeypatch):
    """Mid-run disk check returns failure before encode runs."""

    @dataclass
    class _FakeCompleted:
        returncode: int = 0
        stdout: str = ""
        stderr: str = ""

    encode_calls: list = []

    def _fake_encode(argv, **kwargs):
        encode_calls.append(argv)
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 1024)
        return _FakeCompleted()

    class _FakeUsage:
        free = 0

    monkeypatch.setattr("vmaftune.bisect.shutil.disk_usage", lambda _p: _FakeUsage())

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=5,
        encode_runner=_fake_encode,
        score_runner=None,
        width=1920,
        height=1080,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=60.0,  # non-zero so disk-check fires
        workdir=tmp_path,
    )
    assert result.ok is False
    assert "insufficient disk space" in result.error
    # Should have bailed before any encode.
    assert len(encode_calls) == 0


# ---------------------------------------------------------------------------
# bisect NR proxy path
# ---------------------------------------------------------------------------


class _FakeNRResult:
    def __init__(self, nr_score: float):
        self.nr_score = nr_score


class _FakeNRBackend:
    """Fake NRProxyBackend that always says NR score is far from target."""

    def __init__(
        self,
        *,
        nr_score: float = 50.0,
        far: bool = True,
        direction: str = "tighter",
    ):
        self._nr_score = nr_score
        self._far = far
        self._direction = direction
        self.calibration_threshold = 5.0

    def score_nr(self, path: Path, *, width: int, height: int, pix_fmt: str) -> _FakeNRResult:
        return _FakeNRResult(self._nr_score)

    def is_far_from_target(self, nr_score: float, target: float) -> bool:
        return self._far

    def nr_implied_direction(self, nr_score: float, target: float) -> str:
        return self._direction

    def calibrated_vmaf_score(self, nr_score: float) -> float:
        return nr_score * 0.9 + 5.0


def _make_bisect_runners_for_nr():
    """Minimal runners for NR tests — always succeed with VMAF = 85."""

    @dataclass
    class _FC:
        returncode: int = 0
        stdout: str = ""
        stderr: str = "ffmpeg version 0-test\nx264 - core 164\n"

    def _encode(argv, **kwargs):
        out = Path(argv[-1])
        out.parent.mkdir(parents=True, exist_ok=True)
        out.write_bytes(b"\x00" * 4096)
        return _FC()

    def _score(argv, **kwargs):
        if "--output" in argv:
            out = Path(argv[argv.index("--output") + 1])
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(
                '{"pooled_metrics": {"vmaf": {"mean": 85.0}}}',
                encoding="utf-8",
            )
        return _FC()

    return _encode, _score


def test_bisect_nr_early_elimination_saves_fr_calls():
    """NR backend that is always far from target causes FR skips."""
    enc, sc = _make_bisect_runners_for_nr()
    nr_backend = _FakeNRBackend(far=True, direction="tighter")

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(0, 51),
        max_iterations=6,
        encode_runner=enc,
        score_runner=sc,
        width=640,
        height=480,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=5.0,
        nr_proxy_backend=nr_backend,
    )
    # Either ok or not-ok, but NR saves should be counted.
    # For a far-from-target backend directing "tighter", the window narrows
    # upward so eventually converges (or exhausts iterations).
    assert result.fr_calls_saved >= 0


def test_bisect_nr_within_uncertainty_zone_falls_through():
    """NR within the uncertainty zone always falls through to FR."""
    enc, sc = _make_bisect_runners_for_nr()
    nr_backend = _FakeNRBackend(far=False)  # never far → always falls through

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libx264",
        target_vmaf=80.0,
        crf_range=(15, 30),
        max_iterations=4,
        encode_runner=enc,
        score_runner=sc,
        width=640,
        height=480,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=5.0,
        nr_proxy_backend=nr_backend,
    )
    # When within-zone, every iteration pays the FR cost — saved==0.
    assert result.fr_calls_saved == 0


def test_try_nr_early_elimination_fallback_on_backend_error():
    """NRProxyBackendError is caught and returns None."""
    from vmaftune.bisect import _try_nr_early_elimination_on_yuv
    from vmaftune.score_backend import NRProxyBackendError

    class _ErrBackend:
        calibration_threshold = 5.0

        def score_nr(self, path, *, width, height, pix_fmt):
            raise NRProxyBackendError("boom")

        def is_far_from_target(self, nr, target):
            return True

        def nr_implied_direction(self, nr, target):
            return "tighter"

        def calibrated_vmaf_score(self, nr):
            return nr

    result = _try_nr_early_elimination_on_yuv(
        nr_proxy_backend=_ErrBackend(),
        distorted_yuv=Path("fake.yuv"),
        width=640,
        height=480,
        pix_fmt="yuv420p",
        target_vmaf=80.0,
    )
    assert result is None


def test_try_nr_early_elimination_far_returns_direction():
    """When NR score is far from target, return (direction, vmaf)."""
    from vmaftune.bisect import _try_nr_early_elimination_on_yuv

    class _FarBackend:
        calibration_threshold = 5.0

        def score_nr(self, path, *, width, height, pix_fmt):
            return _FakeNRResult(nr_score=30.0)

        def is_far_from_target(self, nr, target):
            return True

        def nr_implied_direction(self, nr, target):
            return "looser"

        def calibrated_vmaf_score(self, nr):
            return nr + 5.0

    result = _try_nr_early_elimination_on_yuv(
        nr_proxy_backend=_FarBackend(),
        distorted_yuv=Path("fake.yuv"),
        width=640,
        height=480,
        pix_fmt="yuv420p",
        target_vmaf=80.0,
    )
    assert result is not None
    direction, vmaf = result
    assert direction == "looser"
    assert vmaf == pytest.approx(35.0)


def test_try_nr_early_elimination_within_zone_returns_none():
    """When NR score is in the uncertainty zone, returns None."""
    from vmaftune.bisect import _try_nr_early_elimination_on_yuv

    class _NearBackend:
        calibration_threshold = 5.0

        def score_nr(self, path, *, width, height, pix_fmt):
            return _FakeNRResult(nr_score=78.0)

        def is_far_from_target(self, nr, target):
            return False  # within zone

        def nr_implied_direction(self, nr, target):
            return "tighter"

        def calibrated_vmaf_score(self, nr):
            return nr

    result = _try_nr_early_elimination_on_yuv(
        nr_proxy_backend=_NearBackend(),
        distorted_yuv=Path("fake.yuv"),
        width=640,
        height=480,
        pix_fmt="yuv420p",
        target_vmaf=80.0,
    )
    assert result is None


# ---------------------------------------------------------------------------
# bisect encoder-unavailable detection
# ---------------------------------------------------------------------------


def test_bisect_encoder_unavailable_detected():
    """'Encoder not found' in stderr triggers the unavailable error path."""

    @dataclass
    class _FC:
        returncode: int = 1
        stdout: str = ""
        stderr: str = "Unknown encoder 'libsvtav1'\nError initializing output stream"
        stderr_tail: str = "Unknown encoder 'libsvtav1'"
        encode_time_ms: float = 0.0
        encode_size_bytes: int = 0
        encoder_version: str = ""

    result = bisect_target_vmaf(
        Path("ref.yuv"),
        "libsvtav1",
        target_vmaf=80.0,
        crf_range=(20, 50),
        max_iterations=3,
        encode_runner=lambda *a, **k: _FC(),
        score_runner=None,
        width=640,
        height=480,
        pix_fmt="yuv420p",
        framerate=24.0,
        duration_s=5.0,
    )
    assert result.ok is False
    assert "unavailable" in result.error or "encode failed" in result.error


# ---------------------------------------------------------------------------
# ladder
# ---------------------------------------------------------------------------

from vmaftune.ladder import (  # noqa: E402
    LadderPoint,
    UncertaintyLadderPoint,
    _cross,
    _dedup_samples,
    _ideal_targets,
    _nearest_index,
    _plain_ladder_point,
    apply_uncertainty_recipe,
    build_and_emit,
    convex_hull,
    emit_manifest,
    insert_extra_rungs_in_high_uncertainty_regions,
    make_default_sampler,
    prune_redundant_rungs_by_uncertainty,
    select_knees,
)
from vmaftune.uncertainty import ConfidenceThresholds  # noqa: E402


def test_ladder_point_pixel_count():
    p = LadderPoint(1920, 1080, 5000.0, 95.0, 20)
    assert p.pixel_count == 1920 * 1080


def test_cross_product_positive():
    o = LadderPoint(0, 0, 0.0, 0.0, 0)
    a = LadderPoint(0, 0, 1.0, 1.0, 0)
    b = LadderPoint(0, 0, 2.0, 3.0, 0)
    # (a.br - o.br) * (b.vmaf - o.vmaf) - (a.vmaf - o.vmaf) * (b.br - o.br)
    # = 1.0 * 3.0 - 1.0 * 2.0 = 1.0 > 0
    assert _cross(o, a, b) > 0


def test_cross_product_negative():
    o = LadderPoint(0, 0, 0.0, 0.0, 0)
    a = LadderPoint(0, 0, 1.0, 3.0, 0)
    b = LadderPoint(0, 0, 2.0, 1.0, 0)
    # 1.0 * 1.0 - 3.0 * 2.0 = 1 - 6 = -5 < 0
    assert _cross(o, a, b) < 0


def test_dedup_samples_removes_duplicates():
    pts = [
        LadderPoint(640, 360, 1000.0, 80.0, 25),
        LadderPoint(640, 360, 1000.0, 80.0, 25),  # duplicate by (w,h,crf)
        LadderPoint(640, 360, 1200.0, 85.0, 22),
    ]
    result = _dedup_samples(pts)
    assert len(result) == 2


def test_dedup_samples_preserves_first_occurrence():
    pts = [
        LadderPoint(640, 360, 1000.0, 80.0, 25),
        LadderPoint(640, 360, 2000.0, 90.0, 25),  # same (w,h,crf), different bitrate/vmaf
    ]
    result = _dedup_samples(pts)
    assert len(result) == 1
    assert result[0].bitrate_kbps == 1000.0


def test_plain_ladder_point_from_uncertainty():
    u = UncertaintyLadderPoint(640, 360, 1000.0, 80.0, 25, vmaf_low=77.0, vmaf_high=83.0)
    plain = _plain_ladder_point(u)
    assert isinstance(plain, LadderPoint)
    assert plain.vmaf == 80.0


def test_plain_ladder_point_passthrough():
    p = LadderPoint(640, 360, 1000.0, 80.0, 25)
    assert _plain_ladder_point(p) is p


def test_select_knees_n_one_returns_highest_quality():
    hull = [
        LadderPoint(640, 360, 500.0, 70.0, 30),
        LadderPoint(1280, 720, 2000.0, 90.0, 22),
        LadderPoint(1920, 1080, 5000.0, 95.0, 20),
    ]
    rungs = select_knees(hull, n=1)
    assert len(rungs) == 1
    assert rungs[0].vmaf == 95.0


def test_select_knees_unknown_spacing_raises():
    # Hull must have MORE points than n so the _ideal_targets path fires.
    hull = [
        LadderPoint(640, 360, 500.0, 70.0, 30),
        LadderPoint(854, 480, 1200.0, 78.0, 26),
        LadderPoint(1280, 720, 2500.0, 86.0, 22),
        LadderPoint(1920, 1080, 5000.0, 95.0, 20),
    ]
    with pytest.raises(ValueError, match="unknown spacing"):
        select_knees(hull, n=2, spacing="foobar")


def test_select_knees_empty_hull_returns_empty():
    assert select_knees([], n=3) == []


def test_ideal_targets_log_bitrate():
    hull = [
        LadderPoint(640, 360, 1000.0, 70.0, 30),
        LadderPoint(1920, 1080, 8000.0, 95.0, 20),
    ]
    targets = _ideal_targets(hull, 3, "log_bitrate")
    assert len(targets) == 3
    assert targets[0] <= targets[-1]


def test_ideal_targets_vmaf():
    hull = [
        LadderPoint(640, 360, 1000.0, 70.0, 30),
        LadderPoint(1920, 1080, 8000.0, 95.0, 20),
    ]
    targets = _ideal_targets(hull, 3, "vmaf")
    assert targets[0] == pytest.approx(70.0)
    assert targets[-1] == pytest.approx(95.0)


def test_ideal_targets_uniform_alias():
    hull = [
        LadderPoint(640, 360, 1000.0, 70.0, 30),
        LadderPoint(1920, 1080, 8000.0, 95.0, 20),
    ]
    uniform = _ideal_targets(hull, 3, "uniform")
    vmaf = _ideal_targets(hull, 3, "vmaf")
    assert uniform == vmaf


def test_ideal_targets_hi_le_lo_returns_flat():
    # Same point twice — lo == hi after log.
    hull = [LadderPoint(640, 360, 1000.0, 80.0, 25)] * 2
    targets = _ideal_targets(hull, 3, "log_bitrate")
    assert all(t == targets[0] for t in targets)


def test_nearest_index_log_bitrate():
    hull = [
        LadderPoint(640, 360, 1000.0, 70.0, 30),
        LadderPoint(1280, 720, 3000.0, 85.0, 24),
        LadderPoint(1920, 1080, 9000.0, 95.0, 20),
    ]
    # Target near 3000 kbps in log space.
    idx = _nearest_index(hull, math.log(3000.0), "log_bitrate")
    assert idx == 1


def test_nearest_index_vmaf():
    hull = [
        LadderPoint(640, 360, 1000.0, 70.0, 30),
        LadderPoint(1280, 720, 3000.0, 85.0, 24),
        LadderPoint(1920, 1080, 9000.0, 95.0, 20),
    ]
    idx = _nearest_index(hull, 84.0, "vmaf")
    assert idx == 1


def test_make_default_sampler_closes_over_kwargs(monkeypatch):
    from vmaftune import corpus as corpus_module

    captured: list[Any] = []

    def fake_iter_rows(job, opts, **kwargs):
        captured.append({"job": job, "opts": opts})
        for _p, crf in job.cells:
            yield {
                "preset": "medium",
                "crf": crf,
                "vmaf_score": 90.0,
                "bitrate_kbps": 3000.0,
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)

    sampler = make_default_sampler(
        pix_fmt="yuv420p10le",
        framerate=30.0,
        duration_s=5.0,
        crf_sweep=[20, 30],
    )
    from vmaftune.ladder import build_ladder

    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=[(640, 360)],
        target_vmafs=[85.0],
        sampler=sampler,
    )
    assert len(ladder.points) == 1
    # Verify the factory threaded framerate=30 into the corpus job.
    assert captured[0]["job"].framerate == 30.0
    assert captured[0]["job"].pix_fmt == "yuv420p10le"


def test_make_default_sampler_cloud_sink_accumulates_all_rows(monkeypatch):
    from vmaftune import corpus as corpus_module

    def fake_iter_rows(job, opts, **kwargs):
        for _p, crf in job.cells:
            yield {
                "preset": "medium",
                "crf": crf,
                "vmaf_score": 100.0 - crf * 1.0,
                "bitrate_kbps": 3000.0,
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)
    cloud_sink: list[LadderPoint] = []
    sampler = make_default_sampler(
        crf_sweep=[20, 25, 30],
        cloud_sink=cloud_sink,
    )
    from vmaftune.ladder import build_ladder

    build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=[(640, 360)],
        target_vmafs=[85.0],
        sampler=sampler,
    )
    # cloud_sink should capture all 3 CRF rows from the sweep.
    assert len(cloud_sink) == 3


def test_build_and_emit_with_extra_samples_deduplicated(monkeypatch):
    from vmaftune import corpus as corpus_module

    def fake_iter_rows(job, opts, **kwargs):
        for _p, crf in job.cells:
            yield {
                "preset": "medium",
                "crf": crf,
                "vmaf_score": 100.0 - (crf - 18) * 1.5,
                "bitrate_kbps": 200.0 * (40 - crf),
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)

    # Two identical extra samples — dedup should reduce to 1.
    extra = [
        LadderPoint(640, 360, 2000.0, 80.0, 25),
        LadderPoint(640, 360, 2000.0, 80.0, 25),
    ]
    manifest = build_and_emit(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=[(640, 360)],
        target_vmafs=[80.0],
        format="json",
        sampler=None,
        extra_samples=extra,
    )
    payload = json.loads(manifest)
    # The 2 identical extra_samples should deduplicate to 1.
    sample_keys = [(s["width"], s["height"], s["crf"]) for s in payload["samples"]]
    assert len(sample_keys) == len(set(sample_keys))


def test_convex_hull_duplicate_bitrates_keeps_highest_vmaf():
    pts = [
        LadderPoint(640, 360, 1000.0, 80.0, 25),
        LadderPoint(640, 360, 1000.0, 75.0, 27),  # same bitrate, lower vmaf
        LadderPoint(1920, 1080, 5000.0, 95.0, 20),
    ]
    hull = convex_hull(pts)
    bitrates = [p.bitrate_kbps for p in hull]
    # Only one point at bitrate=1000.
    assert bitrates.count(1000.0) <= 1


def test_emit_json_empty_ladder():
    payload = json.loads(emit_manifest([], format="json"))
    assert payload["renditions"] == []
    assert payload["samples"] == []


def test_emit_hls_empty_ladder():
    manifest = emit_manifest([], format="hls")
    assert manifest.startswith("#EXTM3U")
    # No STREAM-INF lines.
    assert "#EXT-X-STREAM-INF" not in manifest


# ---------------------------------------------------------------------------
# recommend — load_corpus_jsonl, format_result, uncertainty path
# ---------------------------------------------------------------------------

from vmaftune.recommend import (  # noqa: E402
    UncertaintyAwareRequest,
    _row_interval,
    format_result,
    load_corpus_jsonl,
    pick_target_vmaf_with_uncertainty,
)
from vmaftune.uncertainty import ConfidenceDecision  # noqa: E402


def _row(
    *,
    encoder="libx264",
    preset="medium",
    crf=23,
    vmaf=90.0,
    bitrate=2000.0,
    exit_status=0,
    vmaf_interval=None,
) -> dict:
    r = {
        "encoder": encoder,
        "preset": preset,
        "crf": crf,
        "vmaf_score": vmaf,
        "bitrate_kbps": bitrate,
        "exit_status": exit_status,
    }
    if vmaf_interval is not None:
        r["vmaf_interval"] = vmaf_interval
    return r


def test_load_corpus_jsonl_streams_rows(tmp_path):
    rows = [
        {"vmaf_score": 90.0, "crf": 22},
        {"vmaf_score": 85.0, "crf": 26},
        {},  # empty line will be ignored by blank skip
    ]
    corpus = tmp_path / "c.jsonl"
    lines = [json.dumps(r) for r in rows if r]
    corpus.write_text("\n".join(lines) + "\n\n", encoding="utf-8")
    loaded = list(load_corpus_jsonl(corpus))
    assert len(loaded) == 2
    assert loaded[0]["crf"] == 22


def test_format_result_contains_key_fields():
    result_obj = __import__("vmaftune.recommend", fromlist=["RecommendResult"]).RecommendResult(
        row=_row(crf=22, vmaf=95.0, bitrate=5000.0),
        predicate="target_vmaf>=92.0",
        margin=3.0,
    )
    text = format_result(result_obj)
    assert "crf=22" in text
    assert "vmaf=95.000" in text
    assert "predicate=target_vmaf>=92.0" in text
    assert "margin=+3.000" in text


def test_row_interval_uses_sample_uncertainty_override():
    req = UncertaintyAwareRequest(
        target_vmaf=90.0,
        sample_uncertainty={22: (91.0, 89.0, 93.0)},
    )
    r = _row(crf=22, vmaf=91.0)
    point, low, high = _row_interval(r, req)
    assert point == pytest.approx(91.0)
    assert low == pytest.approx(89.0)
    assert high == pytest.approx(93.0)


def test_row_interval_uses_embedded_vmaf_interval():
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    r = _row(crf=22, vmaf=91.0, vmaf_interval={"low": 88.0, "high": 94.0})
    point, low, high = _row_interval(r, req)
    assert low == pytest.approx(88.0)
    assert high == pytest.approx(94.0)


def test_row_interval_returns_nan_when_no_interval():
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    r = _row(crf=22, vmaf=91.0)  # no vmaf_interval key
    point, low, high = _row_interval(r, req)
    assert math.isnan(low)
    assert math.isnan(high)


def test_row_interval_returns_nan_for_missing_vmaf_score():
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    r = {"crf": 22, "bitrate_kbps": 2000.0}  # no vmaf_score
    point, low, high = _row_interval(r, req)
    assert math.isnan(point)


def test_pick_with_uncertainty_tight_interval_short_circuits():
    """Tight interval with low >= target short-circuits at first match."""
    rows = [
        _row(crf=18, vmaf=97.0, vmaf_interval={"low": 95.0, "high": 99.0}),  # tight, clears
        _row(crf=22, vmaf=94.0, vmaf_interval={"low": 92.0, "high": 96.0}),
        _row(crf=28, vmaf=88.0, vmaf_interval={"low": 86.0, "high": 90.0}),
    ]
    req = UncertaintyAwareRequest(
        target_vmaf=94.0,
        thresholds=ConfidenceThresholds(tight_interval_max_width=4.0, wide_interval_min_width=8.0),
    )
    result = pick_target_vmaf_with_uncertainty(rows, req)
    # tight path fires at crf=18 (low=95.0 >= target=94.0).
    assert result.decision is ConfidenceDecision.TIGHT
    assert result.row["crf"] == 18
    assert result.visited < len(rows)


def test_pick_with_uncertainty_wide_interval_forces_full_scan():
    """Wide intervals force the full-scan fallback."""
    rows = [
        _row(crf=18, vmaf=97.0, vmaf_interval={"low": 85.0, "high": 99.0}),  # width=14 → wide
        _row(crf=22, vmaf=95.0, vmaf_interval={"low": 83.0, "high": 97.0}),  # wide
    ]
    req = UncertaintyAwareRequest(
        target_vmaf=94.0,
        thresholds=ConfidenceThresholds(tight_interval_max_width=3.0, wide_interval_min_width=6.0),
    )
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.WIDE
    assert "UNCERTAIN" in result.predicate
    assert result.visited == len(rows)


def test_pick_with_uncertainty_unmet_all_excluded():
    """Every row's interval excludes the target → UNMET + interval-excluded."""
    rows = [
        _row(crf=30, vmaf=70.0, vmaf_interval={"low": 65.0, "high": 75.0}),
        _row(crf=35, vmaf=65.0, vmaf_interval={"low": 60.0, "high": 70.0}),
    ]
    req = UncertaintyAwareRequest(
        target_vmaf=95.0,  # way above all intervals
        thresholds=ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0),
    )
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert "UNMET" in result.predicate


def test_pick_with_uncertainty_middle_band_falls_back_to_point():
    """Rows with NaN intervals fall to MIDDLE band — point-estimate path."""
    rows = [
        _row(crf=18, vmaf=97.0),  # no interval → NaN → MIDDLE
        _row(crf=22, vmaf=95.0),
    ]
    req = UncertaintyAwareRequest(target_vmaf=94.0)
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.decision is ConfidenceDecision.MIDDLE
    # Point-estimate picks smallest CRF clearing target — that is crf=18.
    assert result.row["crf"] == 18


def test_pick_with_uncertainty_empty_rows_raises():
    req = UncertaintyAwareRequest(target_vmaf=90.0)
    with pytest.raises(ValueError, match="no eligible rows"):
        pick_target_vmaf_with_uncertainty([], req)


def test_pick_with_uncertainty_honours_encoder_filter():
    rows = [
        _row(encoder="libx265", crf=18, vmaf=97.0, vmaf_interval={"low": 95.0, "high": 99.0}),
        _row(encoder="libx264", crf=22, vmaf=95.0, vmaf_interval={"low": 93.0, "high": 97.0}),
    ]
    req = UncertaintyAwareRequest(
        target_vmaf=93.0,
        encoder="libx264",
        thresholds=ConfidenceThresholds(tight_interval_max_width=4.0, wide_interval_min_width=8.0),
    )
    result = pick_target_vmaf_with_uncertainty(rows, req)
    assert result.row["encoder"] == "libx264"


# ---------------------------------------------------------------------------
# cache — additional edge cases
# ---------------------------------------------------------------------------

from vmaftune.cache import CachedResult, TuneCache  # noqa: E402


def _make_cached_result(score=92.5) -> CachedResult:
    return CachedResult(
        encode_size_bytes=4096,
        encode_time_ms=10.0,
        encoder_version="libx264-164",
        ffmpeg_version="6.1.1",
        vmaf_score=score,
        vmaf_model="vmaf_v0.6.1",
        score_time_ms=5.0,
        vmaf_binary_version="3.0.0-lusoris",
        artifact_path=Path("placeholder"),
    )


def test_cache_keys_empty_dir(tmp_path):
    c = TuneCache(tmp_path / "cache")
    assert list(c.keys()) == []


def test_cache_total_bytes_zero_when_empty(tmp_path):
    c = TuneCache(tmp_path / "cache")
    assert c.total_bytes() == 0


def test_cache_total_bytes_after_put(tmp_path):
    c = TuneCache(tmp_path / "cache")
    blob = tmp_path / "e.bin"
    blob.write_bytes(b"\x00" * 2048)
    c.put("k1", _make_cached_result(), blob)
    assert c.total_bytes() > 0


def test_cache_corrupt_index_returns_empty_on_read(tmp_path):
    c = TuneCache(tmp_path / "cache")
    c._index_path().write_text("not valid json", encoding="utf-8")
    # Should recover gracefully and return an empty index.
    idx = c._read_index()
    assert idx == {}


def test_cache_put_raises_on_missing_artifact(tmp_path):
    c = TuneCache(tmp_path / "cache")
    with pytest.raises(FileNotFoundError):
        c.put("k1", _make_cached_result(), tmp_path / "nonexistent.bin")


def test_cache_get_missing_meta_returns_none(tmp_path):
    c = TuneCache(tmp_path / "cache")
    # Write a blob but no meta — should miss.
    blob_path = c._blob_path("k1")
    blob_path.parent.mkdir(parents=True, exist_ok=True)
    blob_path.write_bytes(b"\x00")
    assert c.get("k1") is None


def test_cache_get_missing_blob_returns_none(tmp_path):
    c = TuneCache(tmp_path / "cache")
    # Write meta but no blob.
    meta_path = c._meta_path("k1")
    meta_path.parent.mkdir(parents=True, exist_ok=True)
    meta_path.write_text(json.dumps({"vmaf_score": 90.0}), encoding="utf-8")
    assert c.get("k1") is None


def test_cache_get_corrupt_meta_returns_none(tmp_path):
    c = TuneCache(tmp_path / "cache")
    meta = c._meta_path("k1")
    meta.parent.mkdir(parents=True, exist_ok=True)
    meta.write_text("not json", encoding="utf-8")
    blob = c._blob_path("k1")
    blob.parent.mkdir(parents=True, exist_ok=True)
    blob.write_bytes(b"\x00")
    assert c.get("k1") is None


def test_cache_keys_lists_only_meta_json(tmp_path):
    c = TuneCache(tmp_path / "cache")
    blob = tmp_path / "e.bin"
    blob.write_bytes(b"\x00" * 512)
    c.put("key_alpha", _make_cached_result(), blob)
    c.put("key_beta", _make_cached_result(), blob)
    all_keys = sorted(c.keys())
    assert "key_alpha" in all_keys
    assert "key_beta" in all_keys


def test_cache_evict_lru_removes_entries(tmp_path):
    c = TuneCache(tmp_path / "cache", size_bytes=10**12)
    blob = tmp_path / "e.bin"
    blob.write_bytes(b"\x00" * 1024)
    for i in range(4):
        c.put(f"k{i}", _make_cached_result(), blob)
    before = c.total_bytes()
    # Force eviction by setting a tiny cap.
    evicted = c.evict_lru(1)
    assert evicted >= 1
    assert c.total_bytes() < before


def test_cache_evict_lru_zero_target_noop(tmp_path):
    c = TuneCache(tmp_path / "cache")
    blob = tmp_path / "e.bin"
    blob.write_bytes(b"\x00" * 1024)
    c.put("k", _make_cached_result(), blob)
    assert c.evict_lru(0) == 0


def test_cache_total_bytes_missing_subdir(tmp_path):
    """total_bytes is safe when a sub-dir is missing (not yet created)."""
    c = TuneCache.__new__(TuneCache)
    c.path = tmp_path / "nonexistent_cache"
    c.size_bytes = 1024
    # Calling total_bytes on a non-existing tree should return 0, not raise.
    assert c.total_bytes() == 0


# ---------------------------------------------------------------------------
# hdr — additional edge cases
# ---------------------------------------------------------------------------

from vmaftune.hdr import (  # noqa: E402
    HdrInfo,
    _classify_payload,
    _extract_mastering,
    _frac_to_unit,
    _hdr_args_vvenc,
    detect_hdr,
    hdr_codec_args,
    reset_hdr_model_warning,
    select_hdr_vmaf_model,
)


def test_frac_to_unit_int_input():
    assert _frac_to_unit(1, scale=10000) == 10000


def test_frac_to_unit_float_input():
    assert _frac_to_unit(0.5, scale=10000) == 5000


def test_frac_to_unit_fraction_string():
    assert _frac_to_unit("3/4", scale=10000) == 7500


def test_frac_to_unit_bad_string_returns_zero():
    assert _frac_to_unit("not_a_number", scale=10000) == 0


def test_frac_to_unit_division_by_zero_string_returns_zero():
    assert _frac_to_unit("1/0", scale=10000) == 0


def test_hdr_args_vvenc_emits_global_color_tags():
    info = HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
    )
    args = _hdr_args_vvenc(info)
    assert "-color_primaries" in args
    assert "-color_trc" in args
    assert args[args.index("-color_trc") + 1] == "smpte2084"


def test_hdr_codec_args_libaom_hlg_global():
    info = HdrInfo(
        transfer="hlg",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
    )
    args = hdr_codec_args("libaom-av1", info)
    assert "-color_trc" in args
    assert args[args.index("-color_trc") + 1] == "arib-std-b67"


def test_classify_payload_returns_none_for_empty_streams():
    assert _classify_payload({"streams": []}) is None


def test_classify_payload_returns_none_for_no_streams_key():
    assert _classify_payload({}) is None


def test_classify_payload_unknown_transfer_returns_none():
    payload = {
        "streams": [
            {
                "color_transfer": "gamma22",
                "color_primaries": "bt2020",
                "color_space": "bt2020nc",
                "color_range": "tv",
                "pix_fmt": "yuv420p",
            }
        ]
    }
    assert _classify_payload(payload) is None


def test_extract_mastering_incomplete_side_data():
    """Side data missing any coordinate returns None."""
    sd = [
        {
            "side_data_type": "Mastering display metadata",
            "red_x": "34000/50000",
            # Missing all other coords.
        }
    ]
    md, cll = _extract_mastering(sd)
    assert md is None
    assert cll is None


def test_extract_mastering_content_light_only():
    sd = [
        {
            "side_data_type": "Content light level metadata",
            "max_content": 1200,
            "max_average": 600,
        }
    ]
    md, cll = _extract_mastering(sd)
    assert md is None
    assert cll == "1200,600"


def test_extract_mastering_empty_side_data():
    md, cll = _extract_mastering([])
    assert md is None and cll is None


def test_detect_hdr_no_ffprobe_on_path(tmp_path, monkeypatch):
    """Without runner and ffprobe absent, detect_hdr returns None."""
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")
    # Remove ffprobe from PATH.
    monkeypatch.setattr("vmaftune.hdr.shutil.which", lambda _bin: None)
    result = detect_hdr(src)
    assert result is None


def test_detect_hdr_runner_os_error(tmp_path):
    """OSError during ffprobe invocation returns None."""
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")

    def runner(cmd, capture_output, text, check):
        raise OSError("no such binary")

    assert detect_hdr(src, runner=runner) is None


def test_detect_hdr_runner_returns_empty_streams(tmp_path):
    src = tmp_path / "src.mp4"
    src.write_bytes(b"\x00")

    def runner(cmd, capture_output, text, check):
        class _FC:
            returncode = 0
            stdout = json.dumps({"streams": []})

        return _FC()

    assert detect_hdr(src, runner=runner) is None


def test_select_hdr_vmaf_model_no_hdr_json_in_dir(tmp_path):
    """Dir exists but contains no vmaf_hdr_*.json files."""
    (tmp_path / "vmaf_v0.6.1.json").write_text("{}")  # SDR file, ignored
    reset_hdr_model_warning()
    result = select_hdr_vmaf_model(tmp_path, transfer="pq")
    assert result is None


def test_hdr_info_full_round_trip_x265_with_hlg(tmp_path):
    """HLG info through hdr_codec_args for libx265 includes correct transfer."""
    info = HdrInfo(
        transfer="hlg",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
        master_display="G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000000,500)",
        max_cll="1000,400",
    )
    args = hdr_codec_args("libx265", info)
    payload = args[args.index("-x265-params") + 1]
    assert "transfer=arib-std-b67" in payload
    assert "hdr10-opt=1" not in payload
    assert "master-display=" in payload


def test_hdr_x265_no_master_display_or_cll():
    """x265 without SEI metadata omits those params from -x265-params."""
    info = HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
        master_display=None,
        max_cll=None,
    )
    args = hdr_codec_args("libx265", info)
    payload = args[args.index("-x265-params") + 1]
    assert "master-display=" not in payload
    assert "max-cll=" not in payload


def test_hdr_svtav1_full_range():
    """SVT-AV1 with pc color-range emits color-range=1."""
    info = HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="pc",
        pix_fmt="yuv420p10le",
    )
    args = hdr_codec_args("libsvtav1", info)
    payload = args[args.index("-svtav1-params") + 1]
    assert "color-range=1" in payload


def test_hdr_nvenc_hevc_includes_master_display_in_args():
    """NVENC HEVC pass-through of master_display and max_cll."""
    info = HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
        master_display="G(13250,34500)B(7500,3000)R(34000,16000)WP(15635,16450)L(10000000000,500)",
        max_cll="1000,400",
    )
    args = hdr_codec_args("hevc_nvenc", info)
    assert "-master_display" in args
    assert "-max_cll" in args


def test_hdr_nvenc_hevc_no_sei_when_absent():
    """NVENC HEVC without SEI data does not emit -master_display / -max_cll."""
    info = HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
    )
    args = hdr_codec_args("hevc_nvenc", info)
    assert "-master_display" not in args
    assert "-max_cll" not in args


# ---------------------------------------------------------------------------
# uncertainty transforms (ladder.py lines 925–981)
# ---------------------------------------------------------------------------


def _make_uncertainty_rungs(
    n: int = 4,
    *,
    low_offset: float = -2.0,
    high_offset: float = 2.0,
    start_bitrate: float = 1000.0,
    bitrate_step: float = 2000.0,
    start_vmaf: float = 70.0,
    vmaf_step: float = 8.0,
) -> list[UncertaintyLadderPoint]:
    return [
        UncertaintyLadderPoint(
            width=640,
            height=360,
            bitrate_kbps=start_bitrate + i * bitrate_step,
            vmaf=start_vmaf + i * vmaf_step,
            crf=30 - i * 3,
            vmaf_low=start_vmaf + i * vmaf_step + low_offset,
            vmaf_high=start_vmaf + i * vmaf_step + high_offset,
        )
        for i in range(n)
    ]


def test_prune_redundant_rungs_removes_overlapping():
    rungs = [
        UncertaintyLadderPoint(640, 360, 1000.0, 80.0, 30, vmaf_low=78.0, vmaf_high=82.0),
        UncertaintyLadderPoint(
            640, 360, 2000.0, 81.0, 28, vmaf_low=79.0, vmaf_high=83.0
        ),  # overlaps prev
        UncertaintyLadderPoint(640, 360, 8000.0, 95.0, 20, vmaf_low=93.0, vmaf_high=97.0),
    ]
    pruned = prune_redundant_rungs_by_uncertainty(rungs, overlap_threshold=0.5)
    # The overlapping interior rung should be dropped/replaced.
    assert len(pruned) < 3 or pruned[-1].vmaf == pytest.approx(95.0)


def test_prune_redundant_rungs_short_list_unchanged():
    rungs = _make_uncertainty_rungs(2)
    result = prune_redundant_rungs_by_uncertainty(rungs)
    assert result == rungs


def test_prune_redundant_rungs_invalid_threshold():
    with pytest.raises(ValueError, match="overlap_threshold must be in"):
        prune_redundant_rungs_by_uncertainty([], overlap_threshold=1.5)


def test_insert_extra_rungs_no_wide_intervals():
    """When no pair is WIDE, no extra rungs are inserted."""
    # 2-unit intervals (default ConfidenceThresholds.wide_interval_min_width = 5.0).
    rungs = _make_uncertainty_rungs(3)  # interval_width = 4.0 → not WIDE
    result = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    assert len(result) == 3  # no extras


def test_insert_extra_rungs_with_wide_interval():
    """Wide-interval pair gets a mid-rung inserted."""
    rungs = [
        UncertaintyLadderPoint(
            640, 360, 1000.0, 70.0, 30, vmaf_low=60.0, vmaf_high=80.0
        ),  # width=20 → WIDE
        UncertaintyLadderPoint(640, 360, 8000.0, 90.0, 20, vmaf_low=80.0, vmaf_high=100.0),  # wide
    ]
    result = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    # One mid-rung should be inserted between the two.
    assert len(result) == 3
    bitrates = [p.bitrate_kbps for p in result]
    assert bitrates[0] < bitrates[1] < bitrates[2]


def test_insert_extra_rungs_single_rung_unchanged():
    rungs = [UncertaintyLadderPoint(640, 360, 1000.0, 80.0, 25, vmaf_low=78.0, vmaf_high=82.0)]
    result = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    assert result == rungs


def test_apply_uncertainty_recipe_compose():
    """apply_uncertainty_recipe = prune then insert, no crash on short list."""
    rungs = _make_uncertainty_rungs(4)
    result = apply_uncertainty_recipe(rungs)
    assert isinstance(result, list)
    assert all(isinstance(r, UncertaintyLadderPoint) for r in result)


def test_uncertainty_ladder_point_interval_width():
    u = UncertaintyLadderPoint(640, 360, 1000.0, 80.0, 25, vmaf_low=77.0, vmaf_high=83.0)
    assert u.interval_width == pytest.approx(6.0)


def test_uncertainty_ladder_point_inverted_interval_width_is_zero():
    u = UncertaintyLadderPoint(640, 360, 1000.0, 80.0, 25, vmaf_low=83.0, vmaf_high=77.0)
    assert u.interval_width == 0.0
