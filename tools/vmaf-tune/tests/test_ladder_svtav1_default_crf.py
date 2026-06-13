# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Regression tests for Bug N-2: ``ladder --encoder libsvtav1`` exited 2 on
the default CRF sweep.

Root cause: ``DEFAULT_SAMPLER_CRF_SWEEP`` started at CRF 18, which is below
libsvtav1's Phase A lower bound of 20 (``SvtAv1Adapter.quality_range = (20,
50)``).  ``corpus.iter_rows`` calls ``adapter.validate(preset, crf)`` for
every cell before encoding starts; for CRF 18 this raised ``ValueError: crf
18 outside Phase A range [20, 50]``, which the ladder converted to an exit-2
without any encodes running.

No real ffmpeg / vmaf binaries are required — the adapter validation logic is
pure-Python and exercised directly here.
"""

from __future__ import annotations

import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.codec_adapters import get_adapter  # noqa: E402
from vmaftune.ladder import DEFAULT_SAMPLER_CRF_SWEEP  # noqa: E402

# ---------------------------------------------------------------------------
# Bug N-2 — DEFAULT_SAMPLER_CRF_SWEEP must be within all adapter Phase A ranges
# ---------------------------------------------------------------------------


def test_default_crf_sweep_within_svtav1_quality_range() -> None:
    """Every point in DEFAULT_SAMPLER_CRF_SWEEP must lie inside libsvtav1's Phase A range.

    This is the direct regression test: before the fix, CRF 18 was in the
    sweep but outside ``SvtAv1Adapter.quality_range = (20, 50)``, causing an
    immediate ``ValueError`` (exit 2) when the ladder was invoked with
    ``--encoder libsvtav1`` without an explicit ``--crf-sweep`` override.
    """
    adapter = get_adapter("libsvtav1")
    lo, hi = adapter.quality_range
    for crf in DEFAULT_SAMPLER_CRF_SWEEP:
        assert lo <= crf <= hi, (
            f"DEFAULT_SAMPLER_CRF_SWEEP point {crf} is outside libsvtav1 "
            f"Phase A range [{lo}, {hi}].  The default sweep must stay within "
            f"every adapter's quality_range so ``ladder --encoder libsvtav1`` "
            f"does not exit 2 without an explicit --crf-sweep override."
        )


def test_default_crf_sweep_start_is_at_least_20() -> None:
    """The sweep must start at >= 20 (libsvtav1 lower bound)."""
    assert DEFAULT_SAMPLER_CRF_SWEEP[0] >= 20, (
        f"DEFAULT_SAMPLER_CRF_SWEEP[0] = {DEFAULT_SAMPLER_CRF_SWEEP[0]}, "
        f"expected >= 20 (libsvtav1 Phase A lower bound)"
    )


def test_svtav1_adapter_validates_each_default_sweep_point() -> None:
    """``adapter.validate()`` must not raise for any default sweep CRF."""
    adapter = get_adapter("libsvtav1")
    # Use the adapter's first preset (or a known good preset string).
    presets = tuple(adapter.presets)
    assert presets, "libsvtav1 adapter declares no presets"
    preset = presets[0]
    for crf in DEFAULT_SAMPLER_CRF_SWEEP:
        # Should not raise.
        adapter.validate(preset, crf)


def test_default_crf_sweep_has_five_points() -> None:
    """Sweep length is preserved (5 points) after the lower-bound shift."""
    assert len(DEFAULT_SAMPLER_CRF_SWEEP) == 5, (
        f"Expected 5 CRF sweep points, got {len(DEFAULT_SAMPLER_CRF_SWEEP)}: "
        f"{DEFAULT_SAMPLER_CRF_SWEEP}"
    )


def test_default_crf_sweep_is_strictly_increasing() -> None:
    """Sweep points must be strictly increasing (monotone grid)."""
    pts = DEFAULT_SAMPLER_CRF_SWEEP
    for i in range(len(pts) - 1):
        assert pts[i] < pts[i + 1], (
            f"DEFAULT_SAMPLER_CRF_SWEEP is not strictly increasing at index {i}: "
            f"{pts[i]} >= {pts[i + 1]}"
        )
