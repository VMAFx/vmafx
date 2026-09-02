# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase E smoke tests — convex hull, knee selection, manifest emit.

Mocks the corpus generator so neither ffmpeg nor vmaf binaries are
required. The synthetic (bitrate, vmaf) cloud is shaped so the
expected Pareto-optimal subset is hand-derivable.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path

import pytest

# Make src/ importable without an editable install.
_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.cli import _build_parser
from vmaftune.ladder import (
    Ladder,
    LadderPoint,
    Rendition,
    UncertaintyLadderPoint,
    build_and_emit,
    build_ladder,
    convex_hull,
    emit_manifest,
    select_knees,
)
from vmaftune.uncertainty import ConfidenceThresholds

# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------


# Canonical 5-rung ABR ladder set (1080p / 720p / 480p / 360p / 240p).
CANONICAL_RES = [
    (1920, 1080),
    (1280, 720),
    (854, 480),
    (640, 360),
    (426, 240),
]

CANONICAL_VMAFS = [95.0, 90.0, 85.0, 75.0, 65.0]


def synthetic_sampler(src: Path, encoder: str, w: int, h: int, target_vmaf: float) -> LadderPoint:
    """Synthetic per-title model shaped like the Netflix paper's R-D curves.

    Each resolution has its own concave (bitrate, vmaf) curve. The
    crucial property: at *low* bitrates, lower resolutions dominate
    the upper hull (a 240p frame at 200 kbps beats a 1080p frame at
    200 kbps); at *high* bitrates, higher resolutions take over. The
    crossings between curves are exactly the "knees" the hull is
    meant to surface.

    Model: vmaf_achieved = ceiling - alpha / bitrate, so
    bitrate = alpha / (ceiling - vmaf). ``ceiling`` rises with
    resolution; ``alpha`` rises with pixel count (a 1080p frame
    needs more bits per VMAF point than a 240p frame).
    """
    pixels = w * h
    if pixels >= 1920 * 1080:
        ceiling, alpha = 100.0, 12000.0
    elif pixels >= 1280 * 720:
        ceiling, alpha = 98.0, 5500.0
    elif pixels >= 854 * 480:
        ceiling, alpha = 95.0, 2500.0
    elif pixels >= 640 * 360:
        ceiling, alpha = 92.0, 1100.0
    else:
        ceiling, alpha = 89.0, 500.0

    # If the target is unreachable for this resolution, achieved sits
    # below the ceiling at a finite bitrate (returns negative gap, so
    # we cap and treat as "best this resolution can do").
    achieved = min(target_vmaf, ceiling - 1.0)
    gap = ceiling - achieved
    bitrate = alpha / gap
    crf = max(18, min(40, round(50 - achieved / 2.0)))
    return LadderPoint(
        width=w,
        height=h,
        bitrate_kbps=round(bitrate, 3),
        vmaf=round(achieved, 3),
        crf=crf,
    )


# ---------------------------------------------------------------------------
# convex_hull
# ---------------------------------------------------------------------------


def test_convex_hull_drops_dominated_interior_points():
    pts = [
        LadderPoint(640, 360, 500.0, 70.0, 30),
        LadderPoint(640, 360, 1000.0, 80.0, 25),  # on hull
        LadderPoint(640, 360, 1500.0, 79.0, 24),  # dominated by the previous
        LadderPoint(1280, 720, 2000.0, 90.0, 22),  # on hull
        LadderPoint(1280, 720, 4000.0, 95.0, 20),  # on hull
    ]
    hull = convex_hull(pts)
    bitrates = [p.bitrate_kbps for p in hull]
    vmafs = [p.vmaf for p in hull]
    # The 1500.0/79.0 point must be off the hull (dominated).
    assert 1500.0 not in bitrates
    # Hull must be monotonically increasing in both bitrate and vmaf
    # (upper Pareto frontier).
    assert bitrates == sorted(bitrates)
    assert vmafs == sorted(vmafs)


def test_convex_hull_synthetic_corpus_picks_pareto_subset():
    pts: list[LadderPoint] = []
    for w, h in CANONICAL_RES:
        for tv in CANONICAL_VMAFS:
            pts.append(synthetic_sampler(Path("dummy"), "libx264", w, h, tv))

    hull = convex_hull(pts)

    # Hull is a strict subset of the input cloud.
    assert 0 < len(hull) < len(pts)
    # Hull is monotonic in both axes.
    bitrates = [p.bitrate_kbps for p in hull]
    vmafs = [p.vmaf for p in hull]
    assert bitrates == sorted(bitrates)
    assert vmafs == sorted(vmafs)
    # No point in the input cloud strictly dominates any hull point.
    for hp in hull:
        for cand in pts:
            if cand is hp:
                continue
            dominates = (cand.bitrate_kbps <= hp.bitrate_kbps and cand.vmaf > hp.vmaf) or (
                cand.bitrate_kbps < hp.bitrate_kbps and cand.vmaf >= hp.vmaf
            )
            assert not dominates, f"{cand} dominates hull point {hp}"


def test_convex_hull_empty_input():
    assert convex_hull([]) == []


# ---------------------------------------------------------------------------
# build_ladder
# ---------------------------------------------------------------------------


def test_build_ladder_visits_full_grid():
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        sampler=synthetic_sampler,
    )
    assert isinstance(ladder, Ladder)
    assert len(ladder.points) == len(CANONICAL_RES) * len(CANONICAL_VMAFS)
    assert ladder.encoder == "libx264"


def test_build_ladder_default_sampler_uses_corpus_and_recommend(monkeypatch):
    """Default sampler composes ``iter_rows`` + ``pick_target_vmaf``.

    Wiring closes the Phase B/E gap: ``_default_sampler`` no longer
    raises ``NotImplementedError`` — it drives a 5-point CRF sweep via
    :func:`vmaftune.corpus.iter_rows` and picks the row closest to
    ``target_vmaf`` via :func:`vmaftune.recommend.pick_target_vmaf`.
    Live encoder runs are stubbed by monkeypatching ``iter_rows`` to
    yield synthetic rows.
    """
    from vmaftune import corpus as corpus_module
    from vmaftune import ladder as ladder_module

    captured_cells: list[tuple[str, int]] = []

    def fake_iter_rows(job, opts, **_kwargs):
        captured_cells.extend(job.cells)
        # Synthetic R-D curve: lower CRF -> higher VMAF, higher bitrate.
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": 100.0 - (crf - 18) * 1.5,
                "bitrate_kbps": 100.0 * (40 - crf),
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)
    # ``_default_sampler`` does ``from .corpus import iter_rows`` at
    # call time, so the patched attribute on the corpus module is the
    # function it actually invokes.

    pt = ladder_module._default_sampler(Path("dummy.yuv"), "libx264", 640, 360, target_vmaf=92.0)
    assert isinstance(pt, LadderPoint)
    assert pt.width == 640 and pt.height == 360
    # Expected cells = (20, 25, 30, 35, 40) at preset "medium".  The sweep
    # starts at 20 (not 18) so the same sweep is valid for every shipped
    # adapter — libsvtav1's Phase A lower bound is 20, so 18 raised
    # ``ValueError`` from ``adapter.validate`` before any encode ran.
    # See ``tests/test_ladder_svtav1_default_crf.py`` (Bug N-2 regression).
    assert {c for _p, c in captured_cells} == {20, 25, 30, 35, 40}
    assert all(p == "medium" for p, _c in captured_cells)
    # ``pick_target_vmaf`` returns the smallest CRF whose VMAF clears
    # the target. With target=92.0 and the synthetic curve
    # ``vmaf = 100.0 - (crf - 18) * 1.5``:
    #   CRF=20 -> vmaf=97.0  (clears 92.0)
    #   CRF=25 -> vmaf=89.5  (misses)
    # so the smallest-CRF clearing row wins: CRF=20, VMAF=97.0.
    assert pt.crf == 20
    assert pt.vmaf == pytest.approx(97.0)


def test_build_ladder_default_sampler_no_longer_raises(monkeypatch):
    """``build_ladder(sampler=None)`` runs end-to-end with stubbed encodes."""
    from vmaftune import corpus as corpus_module

    def fake_iter_rows(job, opts, **_kwargs):
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": 100.0 - (crf - 18) * 1.0,
                "bitrate_kbps": 200.0 * (40 - crf),
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=[(640, 360)],
        target_vmafs=[80.0],
    )
    assert len(ladder.points) == 1
    assert isinstance(ladder.points[0], LadderPoint)


def test_build_ladder_default_sampler_preserves_vmaf_interval(monkeypatch):
    """Corpus rows with ``vmaf_interval`` become uncertainty-aware ladder points."""
    from vmaftune import corpus as corpus_module

    def fake_iter_rows(job, opts, **_kwargs):
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": 90.0,
                "vmaf_interval": {"low": 87.0, "high": 93.0, "alpha": 0.05},
                "bitrate_kbps": 2000.0,
                "encoder": opts.encoder,
                "exit_status": 0,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=[(640, 360)],
        target_vmafs=[89.0],
    )
    point = ladder.points[0]
    assert isinstance(point, UncertaintyLadderPoint)
    assert point.vmaf_low == pytest.approx(87.0)
    assert point.vmaf_high == pytest.approx(93.0)


def test_build_ladder_default_sampler_propagates_no_scorable_rows(monkeypatch):
    """Empty / all-failing row stream surfaces a clear RuntimeError."""
    from vmaftune import corpus as corpus_module

    def fake_iter_rows(job, opts, **_kwargs):
        for preset, crf in job.cells:
            yield {
                "preset": preset,
                "crf": crf,
                "vmaf_score": float("nan"),
                "bitrate_kbps": 0.0,
                "encoder": opts.encoder,
                "exit_status": 1,
            }

    monkeypatch.setattr(corpus_module, "iter_rows", fake_iter_rows)
    with pytest.raises(RuntimeError, match="no scorable encodes"):
        build_ladder(
            src=Path("foo.yuv"),
            encoder="libx264",
            resolutions=[(640, 360)],
            target_vmafs=[80.0],
        )


# ---------------------------------------------------------------------------
# select_knees
# ---------------------------------------------------------------------------


def test_select_knees_returns_n_distinct_rungs_in_ascending_bitrate():
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        sampler=synthetic_sampler,
    )
    hull = convex_hull(ladder.points)
    rungs = select_knees(hull, n=5)
    assert len(rungs) == 5
    bitrates = [r.bitrate_kbps for r in rungs]
    assert bitrates == sorted(bitrates)
    # All rungs must be distinct.
    assert len(set(bitrates)) == 5


def test_select_knees_clamps_when_hull_smaller_than_n():
    hull = [
        LadderPoint(640, 360, 500.0, 70.0, 30),
        LadderPoint(1280, 720, 2000.0, 90.0, 22),
    ]
    rungs = select_knees(hull, n=5)
    assert len(rungs) == 2


def test_select_knees_rejects_zero():
    with pytest.raises(ValueError):
        select_knees([LadderPoint(640, 360, 500.0, 70.0, 30)], n=0)


def test_select_knees_vmaf_spacing_picks_distinct_quality_levels():
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        sampler=synthetic_sampler,
    )
    hull = convex_hull(ladder.points)
    rungs = select_knees(hull, n=3, spacing="vmaf")
    vmafs = [r.vmaf for r in rungs]
    assert len(set(vmafs)) == 3
    assert vmafs == sorted(vmafs)


def test_select_knees_uniform_alias_matches_vmaf_spacing():
    ladder = build_ladder(
        src=Path("foo.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        sampler=synthetic_sampler,
    )
    hull = convex_hull(ladder.points)
    vmaf_rungs = select_knees(hull, n=3, spacing="vmaf")
    uniform_rungs = select_knees(hull, n=3, spacing="uniform")
    assert uniform_rungs == vmaf_rungs


def test_cli_accepts_documented_ladder_vmaf_spacing():
    parser = _build_parser()
    args = parser.parse_args(
        [
            "ladder",
            "--src",
            "foo.yuv",
            "--resolutions",
            "1920x1080",
            "--target-vmafs",
            "95",
            "--spacing",
            "vmaf",
        ]
    )
    assert args.spacing == "vmaf"


# ---------------------------------------------------------------------------
# emit_manifest — HLS / DASH / JSON
# ---------------------------------------------------------------------------


def _five_rung_ladder() -> list[Rendition]:
    return [
        Rendition(426, 240, 250.0, 65.0, 35),
        Rendition(640, 360, 600.0, 75.0, 30),
        Rendition(854, 480, 1200.0, 85.0, 27),
        Rendition(1280, 720, 2500.0, 90.0, 24),
        Rendition(1920, 1080, 5000.0, 95.0, 21),
    ]


def test_emit_hls_has_five_distinct_stream_inf_with_monotonic_bandwidth():
    rungs = _five_rung_ladder()
    manifest = emit_manifest(rungs, format="hls")
    assert manifest.startswith("#EXTM3U")
    stream_inf_lines = [
        line for line in manifest.splitlines() if line.startswith("#EXT-X-STREAM-INF")
    ]
    assert len(stream_inf_lines) == 5
    bandwidths = [int(re.search(r"BANDWIDTH=(\d+)", ln).group(1)) for ln in stream_inf_lines]
    assert bandwidths == sorted(bandwidths)
    assert len(set(bandwidths)) == 5
    # Sanity: bps reported = kbps * 1000.
    assert bandwidths[0] == 250_000
    assert bandwidths[-1] == 5_000_000
    # Resolution token present per rung.
    resolutions = [re.search(r"RESOLUTION=(\d+x\d+)", ln).group(1) for ln in stream_inf_lines]
    assert resolutions == ["426x240", "640x360", "854x480", "1280x720", "1920x1080"]


def test_emit_dash_has_five_representations_with_ascending_bandwidth():
    rungs = _five_rung_ladder()
    mpd = emit_manifest(rungs, format="dash")
    assert "<MPD" in mpd
    bandwidths = [int(b) for b in re.findall(r'bandwidth="(\d+)"', mpd)]
    assert len(bandwidths) == 5
    assert bandwidths == sorted(bandwidths)


def test_emit_json_round_trips_and_carries_schema_tag():
    rungs = _five_rung_ladder()
    payload = json.loads(emit_manifest(rungs, format="json"))
    assert payload["schema"] == "vmaf-tune-ladder/v1"
    assert len(payload["renditions"]) == 5
    bandwidths = [r["bandwidth_bps"] for r in payload["renditions"]]
    assert bandwidths == sorted(bandwidths)
    # CRF + VMAF round-trip.
    assert payload["renditions"][-1]["crf"] == 21
    assert payload["renditions"][-1]["vmaf"] == 95.0


def test_emit_unknown_format_raises():
    with pytest.raises(ValueError, match="hls/dash/json"):
        emit_manifest(_five_rung_ladder(), format="rtmp")


# ---------------------------------------------------------------------------
# build_and_emit — end-to-end smoke (no encoding, no scoring)
# ---------------------------------------------------------------------------


def test_build_and_emit_canonical_5_rung_smoke():
    manifest = build_and_emit(
        src=Path("dummy.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        quality_tiers=5,
        format="hls",
        sampler=synthetic_sampler,
    )
    stream_inf = [ln for ln in manifest.splitlines() if ln.startswith("#EXT-X-STREAM-INF")]
    assert len(stream_inf) == 5
    bandwidths = [int(re.search(r"BANDWIDTH=(\d+)", ln).group(1)) for ln in stream_inf]
    # Monotonically increasing — required by HLS spec for variant playlists.
    assert bandwidths == sorted(bandwidths)
    assert len(set(bandwidths)) == 5


def test_build_and_emit_json_format_matches_rung_count():
    manifest = build_and_emit(
        src=Path("dummy.yuv"),
        encoder="libx264",
        resolutions=CANONICAL_RES,
        target_vmafs=CANONICAL_VMAFS,
        quality_tiers=3,
        format="json",
        sampler=synthetic_sampler,
    )
    payload = json.loads(manifest)
    assert len(payload["renditions"]) == 3


def test_build_and_emit_with_uncertainty_inserts_wide_mid_rung():
    def uncertain_sampler(
        _src: Path, _encoder: str, w: int, h: int, target_vmaf: float
    ) -> UncertaintyLadderPoint:
        if target_vmaf < 90.0:
            return UncertaintyLadderPoint(
                width=w,
                height=h,
                bitrate_kbps=1000.0,
                vmaf=80.0,
                crf=30,
                vmaf_low=77.0,
                vmaf_high=83.0,
            )
        return UncertaintyLadderPoint(
            width=w,
            height=h,
            bitrate_kbps=8000.0,
            vmaf=95.0,
            crf=20,
            vmaf_low=92.0,
            vmaf_high=98.0,
        )

    manifest = build_and_emit(
        src=Path("dummy.yuv"),
        encoder="libx264",
        resolutions=[(1920, 1080)],
        target_vmafs=[80.0, 95.0],
        quality_tiers=5,
        format="json",
        sampler=uncertain_sampler,
        with_uncertainty=True,
    )
    payload = json.loads(manifest)
    bitrates = [r["bitrate_kbps"] for r in payload["renditions"]]
    assert len(bitrates) == 3
    assert any(1000.0 < b < 8000.0 for b in bitrates)


def test_build_and_emit_with_uncertainty_widens_point_only_rows():
    def point_sampler(_src: Path, _encoder: str, w: int, h: int, target_vmaf: float) -> LadderPoint:
        if target_vmaf < 90.0:
            return LadderPoint(
                width=w,
                height=h,
                bitrate_kbps=1000.0,
                vmaf=80.0,
                crf=30,
            )
        return LadderPoint(
            width=w,
            height=h,
            bitrate_kbps=8000.0,
            vmaf=95.0,
            crf=20,
        )

    manifest = build_and_emit(
        src=Path("dummy.yuv"),
        encoder="libx264",
        resolutions=[(1920, 1080)],
        target_vmafs=[80.0, 95.0],
        quality_tiers=5,
        format="json",
        sampler=point_sampler,
        with_uncertainty=True,
        uncertainty_thresholds=ConfidenceThresholds(wide_interval_min_width=4.0),
    )
    payload = json.loads(manifest)
    bitrates = [r["bitrate_kbps"] for r in payload["renditions"]]
    assert len(bitrates) == 3
    assert any(1000.0 < b < 8000.0 for b in bitrates)
