# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the uncertainty-aware ladder transforms (ADR-0279).

Exercises:

* :func:`vmaftune.ladder.prune_redundant_rungs_by_uncertainty` —
  drops adjacent rungs whose conformal intervals overlap above the
  threshold.
* :func:`vmaftune.ladder.insert_extra_rungs_in_high_uncertainty_regions`
  — inserts mid-rungs where the pair-averaged interval width is in
  the WIDE band.
* :func:`vmaftune.ladder.apply_uncertainty_recipe` — the composed
  prune-then-insert transform.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.ladder import (  # noqa: E402
    DEFAULT_RUNG_OVERLAP_THRESHOLD,
    UncertaintyLadderPoint,
    apply_uncertainty_recipe,
    insert_extra_rungs_in_high_uncertainty_regions,
    prune_redundant_rungs_by_uncertainty,
)
from vmaftune.uncertainty import ConfidenceThresholds  # noqa: E402


def _ulp(
    bitrate: float,
    vmaf: float,
    *,
    low: float | None = None,
    high: float | None = None,
    width: int = 1920,
    height: int = 1080,
    crf: int = 23,
) -> UncertaintyLadderPoint:
    if low is None or high is None:
        low = vmaf
        high = vmaf
    return UncertaintyLadderPoint(
        width=width,
        height=height,
        bitrate_kbps=bitrate,
        vmaf=vmaf,
        crf=crf,
        vmaf_low=low,
        vmaf_high=high,
    )


# ---------------------------------------------------------------------------
# prune_redundant_rungs_by_uncertainty
# ---------------------------------------------------------------------------


def test_prune_drops_overlapping_adjacent_rungs():
    """Two overlapping middle rungs collapse to the higher-quality one."""
    rungs = [
        _ulp(1000, 80.0, low=79.0, high=81.0),
        _ulp(2000, 91.0, low=90.0, high=92.0),  # overlaps heavily with neighbour
        _ulp(2200, 91.5, low=90.5, high=92.5),  # ~ identical to rung 2
        _ulp(5000, 96.0, low=95.5, high=96.5),
    ]
    pruned = prune_redundant_rungs_by_uncertainty(rungs)
    # The two middle rungs overlap on >50 % of the wider width.
    # Anchor (1000) and tail (5000) survive; one of the duplicates is dropped.
    bitrates = [r.bitrate_kbps for r in pruned]
    assert 1000 in bitrates
    assert 5000 in bitrates
    assert len(pruned) < len(rungs), "at least one redundant rung must be pruned"


def test_prune_keeps_disjoint_intervals():
    """Disjoint intervals are never pruned regardless of count."""
    rungs = [
        _ulp(1000, 75.0, low=74.0, high=76.0),
        _ulp(2000, 82.0, low=81.0, high=83.0),
        _ulp(4000, 89.0, low=88.0, high=90.0),
        _ulp(8000, 95.0, low=94.0, high=96.0),
    ]
    pruned = prune_redundant_rungs_by_uncertainty(rungs)
    assert len(pruned) == len(rungs)


def test_prune_short_input_unchanged():
    """``len(rungs) <= 2`` is returned verbatim — there is no interior."""
    rungs = [_ulp(1000, 75.0), _ulp(8000, 95.0)]
    pruned = prune_redundant_rungs_by_uncertainty(rungs)
    assert pruned == list(rungs)


def test_prune_overlap_threshold_validation():
    """``overlap_threshold`` outside ``[0, 1]`` raises."""
    rungs = [_ulp(1000, 75.0), _ulp(2000, 80.0), _ulp(4000, 90.0)]
    with pytest.raises(ValueError, match=r"\[0, 1\]"):
        prune_redundant_rungs_by_uncertainty(rungs, overlap_threshold=1.5)


def test_prune_default_threshold_matches_research_floor():
    """The default overlap threshold is 0.5 (Research-0067)."""
    assert DEFAULT_RUNG_OVERLAP_THRESHOLD == 0.5


# ---------------------------------------------------------------------------
# insert_extra_rungs_in_high_uncertainty_regions
# ---------------------------------------------------------------------------


def test_insert_adds_midpoint_when_pair_width_is_wide():
    """A wide-interval gap gets a synthetic midpoint rung."""
    rungs = [
        _ulp(1000, 80.0, low=77.0, high=83.0),  # width=6
        _ulp(8000, 95.0, low=92.0, high=98.0),  # width=6 -> avg 6 (WIDE)
    ]
    augmented = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    assert len(augmented) == 3
    mid = augmented[1]
    # Geometric midpoint on bitrate, arithmetic on VMAF.
    assert 1000 < mid.bitrate_kbps < 8000
    assert 80 < mid.vmaf < 95


def test_insert_skips_tight_pairs():
    """Tight-pair gaps are left alone."""
    rungs = [
        _ulp(1000, 80.0, low=79.5, high=80.5),  # width=1
        _ulp(8000, 95.0, low=94.5, high=95.5),  # width=1 -> avg 1 (TIGHT)
    ]
    augmented = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    assert augmented == list(rungs)


def test_insert_short_input_passthrough():
    """Single-rung input is returned verbatim."""
    rungs = [_ulp(1000, 80.0)]
    assert insert_extra_rungs_in_high_uncertainty_regions(rungs) == list(rungs)


def test_insert_honours_custom_thresholds():
    """A tighter ``wide_interval_min_width`` forces more insertions."""
    rungs = [
        _ulp(1000, 80.0, low=78.5, high=81.5),  # width=3
        _ulp(8000, 95.0, low=93.5, high=96.5),  # width=3 -> avg 3
    ]
    # Default thresholds (2/5): width=3 is in MIDDLE -> no insert.
    default_aug = insert_extra_rungs_in_high_uncertainty_regions(rungs)
    assert len(default_aug) == 2
    # Tighter wide gate (1.5/2.5): width=3 is WIDE -> insert.
    tight_thresholds = ConfidenceThresholds(
        tight_interval_max_width=1.5,
        wide_interval_min_width=2.5,
        source="test",
    )
    tight_aug = insert_extra_rungs_in_high_uncertainty_regions(rungs, thresholds=tight_thresholds)
    assert len(tight_aug) == 3


# ---------------------------------------------------------------------------
# apply_uncertainty_recipe (prune -> insert composition)
# ---------------------------------------------------------------------------


def test_apply_recipe_prunes_then_inserts():
    """Composition prunes overlap first, then inserts wide-gap mid-rungs."""
    rungs = [
        # Two near-duplicates at the low end (will be pruned).
        # Both have wide intervals so after pruning the surviving
        # low rung still pairs with the top rung at avg width=6.
        _ulp(1000, 80.0, low=77.0, high=83.0),  # width=6
        _ulp(1100, 80.5, low=77.5, high=83.5),  # near-duplicate of ^
        # Wide top rung; averaged width post-prune is (6+6)/2=6 (WIDE).
        _ulp(8000, 95.0, low=92.0, high=98.0),
    ]
    out = apply_uncertainty_recipe(rungs)
    bitrates = [r.bitrate_kbps for r in out]
    # Wide gap insertion has run.
    assert any(
        max(1000, 1100) < b < 8000 for b in bitrates
    ), "insert step must add a midpoint between the surviving low-rung and 8000"


def test_recipe_explicit_per_call_sample_intervals():
    """Caller-supplied per-rung intervals drive the recipe deterministically."""
    rungs = [
        _ulp(800, 70.0, low=68.0, high=72.0),  # width 4
        _ulp(2000, 85.0, low=83.0, high=87.0),  # width 4 -> avg 4 (MIDDLE)
        _ulp(8000, 96.0, low=90.0, high=99.0),  # width 9 -> with 85 avg 6.5 (WIDE)
    ]
    out = apply_uncertainty_recipe(rungs)
    # We expect exactly one synthetic rung inserted between 2000 and 8000.
    bitrates = sorted(r.bitrate_kbps for r in out)
    assert 800 in bitrates and 8000 in bitrates
    assert any(2000 < b < 8000 for b in bitrates)


def test_uncertainty_ladder_point_interval_width_property():
    """``UncertaintyLadderPoint.interval_width`` matches ``high - low``."""
    p = _ulp(1000, 90.0, low=88.0, high=92.5)
    assert p.interval_width == pytest.approx(4.5)
    # Negative-or-clipped intervals report 0.
    weird = _ulp(1000, 90.0, low=92.0, high=88.0)
    assert weird.interval_width == 0.0


def test_uncertainty_ladder_point_as_ladder_point_drops_interval():
    """``as_ladder_point`` projects to the plain Phase-E shape."""
    p = _ulp(1000, 90.0, low=88.0, high=92.0, crf=22)
    plain = p.as_ladder_point()
    assert plain.bitrate_kbps == 1000
    assert plain.vmaf == 90.0
    assert plain.crf == 22
    assert not hasattr(plain, "vmaf_low")
