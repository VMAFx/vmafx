# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the F.3 confidence-aware escalation policy (ADR-0364 §F.3).

F.3 makes the F.2 GOSPEL/FALL_BACK gate continuous by consulting the
conformal interval half-width returned by
:meth:`Predictor.predict_vmaf_with_uncertainty` (ADR-0279). The decision
helper :func:`_confidence_aware_escalation` is a pure function of
``(verdict, interval_width, thresholds)``; the tests mock all three and
assert the override branches fire as documented.

Per CLAUDE.md feedback ``no_test_weakening``: the thresholds are
corpus-derived (calibration sidecar shipped in #488). If a test fails
because a real-world sidecar value disagrees with the documented
defaults, the fix is a recalibration PR — not a threshold loosening
here.
"""

from __future__ import annotations

import json
import math
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.auto import (  # noqa: E402
    DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
    DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
    ConfidenceDecision,
    ConfidenceThresholds,
    SourceMeta,
    _confidence_aware_escalation,
    emit_plan_json,
    load_confidence_thresholds,
    run_auto,
)

# ---------------------------------------------------------------------------
# ConfidenceThresholds invariants.
# ---------------------------------------------------------------------------


def test_thresholds_default_values_match_research_floor() -> None:
    """Defaults are the documented Research-0067 floor (2.0 / 5.0 VMAF)."""
    th = ConfidenceThresholds()
    assert th.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    assert th.wide_interval_min_width == DEFAULT_WIDE_INTERVAL_MIN_WIDTH
    assert th.source == "default"


def test_thresholds_reject_negative() -> None:
    with pytest.raises(ValueError):
        ConfidenceThresholds(tight_interval_max_width=-0.1, wide_interval_min_width=5.0)


def test_thresholds_reject_inverted_pair() -> None:
    # tight must be <= wide; an inverted pair would carve a hole in the
    # decision table where neither override fires.
    with pytest.raises(ValueError):
        ConfidenceThresholds(tight_interval_max_width=5.0, wide_interval_min_width=2.0)


# ---------------------------------------------------------------------------
# _confidence_aware_escalation — decision-table tests.
# ---------------------------------------------------------------------------


@pytest.fixture
def thresholds() -> ConfidenceThresholds:
    return ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)


def test_tight_interval_skips_escalation_even_when_verdict_says_fallback(
    thresholds: ConfidenceThresholds,
) -> None:
    # The headline test from the spec: tight conformal interval +
    # nominal-FALL_BACK -> SKIP_ESCALATION. The predictor was wrong
    # about the verdict but right about the certainty.
    decision = _confidence_aware_escalation(
        verdict="FALL_BACK", interval_width=1.0, thresholds=thresholds
    )
    assert decision is ConfidenceDecision.SKIP_ESCALATION


def test_wide_interval_forces_escalation_even_when_verdict_says_gospel(
    thresholds: ConfidenceThresholds,
) -> None:
    # The mirror test: wide conformal interval + nominal-GOSPEL ->
    # FORCE_ESCALATION. Predictor was wrong about the certainty even
    # though it claimed GOSPEL.
    decision = _confidence_aware_escalation(
        verdict="GOSPEL", interval_width=7.0, thresholds=thresholds
    )
    assert decision is ConfidenceDecision.FORCE_ESCALATION


def test_middle_band_defers_to_native_verdict_fallback(
    thresholds: ConfidenceThresholds,
) -> None:
    # Width strictly between the two gates -> the F.2 binary logic
    # applies: FALL_BACK -> RECOMMEND_ESCALATION.
    decision = _confidence_aware_escalation(
        verdict="FALL_BACK", interval_width=3.5, thresholds=thresholds
    )
    assert decision is ConfidenceDecision.RECOMMEND_ESCALATION


def test_middle_band_defers_to_native_verdict_gospel(
    thresholds: ConfidenceThresholds,
) -> None:
    decision = _confidence_aware_escalation(
        verdict="GOSPEL", interval_width=3.5, thresholds=thresholds
    )
    assert decision is ConfidenceDecision.SKIP_ESCALATION


def test_middle_band_likely_treated_as_skip(
    thresholds: ConfidenceThresholds,
) -> None:
    # LIKELY is grouped with GOSPEL — only FALL_BACK opts into the
    # recommend-escalation branch in the middle band.
    decision = _confidence_aware_escalation(
        verdict="LIKELY", interval_width=3.5, thresholds=thresholds
    )
    assert decision is ConfidenceDecision.SKIP_ESCALATION


def test_none_verdict_in_middle_band_recommends_escalation(
    thresholds: ConfidenceThresholds,
) -> None:
    # No native verdict -> safe default in the middle band is
    # recommend, so the operator notices the missing signal.
    decision = _confidence_aware_escalation(verdict=None, interval_width=3.5, thresholds=thresholds)
    assert decision is ConfidenceDecision.RECOMMEND_ESCALATION


def test_nan_width_defers_to_recommend(thresholds: ConfidenceThresholds) -> None:
    # NaN width is the uncalibrated signal from
    # ConformalPredictor.predict when no calibration is shipped.
    # F.3 must not crash; falls back to RECOMMEND_ESCALATION.
    decision = _confidence_aware_escalation(
        verdict="GOSPEL", interval_width=float("nan"), thresholds=thresholds
    )
    assert decision is ConfidenceDecision.RECOMMEND_ESCALATION


def test_negative_width_rejected(thresholds: ConfidenceThresholds) -> None:
    with pytest.raises(ValueError):
        _confidence_aware_escalation(verdict="GOSPEL", interval_width=-0.1, thresholds=thresholds)


@pytest.mark.parametrize(
    ("width", "expected"),
    [
        (0.0, ConfidenceDecision.SKIP_ESCALATION),
        (2.0, ConfidenceDecision.SKIP_ESCALATION),  # boundary inclusive
        (2.0001, ConfidenceDecision.RECOMMEND_ESCALATION),
        (4.9999, ConfidenceDecision.RECOMMEND_ESCALATION),
        (5.0, ConfidenceDecision.FORCE_ESCALATION),  # boundary inclusive
        (100.0, ConfidenceDecision.FORCE_ESCALATION),
    ],
)
def test_boundary_inclusivity_for_fallback_verdict(
    thresholds: ConfidenceThresholds,
    width: float,
    expected: ConfidenceDecision,
) -> None:
    # When the native verdict is FALL_BACK, the boundary semantics
    # are: tight inclusive -> SKIP, middle -> RECOMMEND, wide
    # inclusive -> FORCE. Using FALL_BACK as the verdict makes the
    # middle-band outcome unambiguous.
    decision = _confidence_aware_escalation(
        verdict="FALL_BACK", interval_width=width, thresholds=thresholds
    )
    assert decision is expected


# ---------------------------------------------------------------------------
# load_confidence_thresholds — sidecar parsing.
# ---------------------------------------------------------------------------


def test_loader_falls_back_on_none(caplog: pytest.LogCaptureFixture) -> None:
    with caplog.at_level("WARNING"):
        th = load_confidence_thresholds(None)
    assert th.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    assert th.wide_interval_min_width == DEFAULT_WIDE_INTERVAL_MIN_WIDTH
    assert th.source == "default"
    assert any("falling back to documented defaults" in r.message for r in caplog.records)


def test_loader_falls_back_on_missing_file(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    missing = tmp_path / "no_such_calibration.json"
    with caplog.at_level("WARNING"):
        th = load_confidence_thresholds(missing)
    assert th.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    assert th.wide_interval_min_width == DEFAULT_WIDE_INTERVAL_MIN_WIDTH


def test_loader_honours_per_corpus_overrides(tmp_path: Path) -> None:
    # Per-corpus override = sidecar with non-default widths. The
    # loader must NOT fall back to defaults when the sidecar is valid.
    sidecar = tmp_path / "corpus_calibration.json"
    sidecar.write_text(
        json.dumps(
            {
                "tight_interval_max_width": 1.4,
                "wide_interval_min_width": 4.6,
                "method": "split-conformal",
            }
        ),
        encoding="utf-8",
    )
    th = load_confidence_thresholds(sidecar)
    assert math.isclose(th.tight_interval_max_width, 1.4)
    assert math.isclose(th.wide_interval_min_width, 4.6)
    assert th.source == str(sidecar)


def test_loader_falls_back_on_malformed_json(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    sidecar = tmp_path / "bad.json"
    sidecar.write_text("{not json}", encoding="utf-8")
    with caplog.at_level("WARNING"):
        th = load_confidence_thresholds(sidecar)
    assert th.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH


def test_loader_falls_back_on_missing_keys(
    tmp_path: Path, caplog: pytest.LogCaptureFixture
) -> None:
    sidecar = tmp_path / "incomplete.json"
    sidecar.write_text(json.dumps({"tight_interval_max_width": 1.5}), encoding="utf-8")
    with caplog.at_level("WARNING"):
        th = load_confidence_thresholds(sidecar)
    assert th.tight_interval_max_width == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH


# ---------------------------------------------------------------------------
# run_auto integration — JSON metadata records per-cell decisions.
# ---------------------------------------------------------------------------


def test_run_auto_smoke_emits_confidence_aware_escalations() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
    )
    cae = plan.metadata["confidence_aware_escalations"]
    assert isinstance(cae, list)
    assert len(cae) >= 1
    entry = cae[0]
    # Schema check — keys are part of the public contract.
    assert set(entry) == {"rung", "codec", "verdict", "interval_width", "decision"}
    # Smoke default is GOSPEL + tight width=1.0 -> SKIP_ESCALATION.
    assert entry["decision"] == ConfidenceDecision.SKIP_ESCALATION.value


def test_run_auto_threshold_block_records_source() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
    )
    th_block = plan.metadata["confidence_thresholds"]
    assert th_block["source"] == "default"
    assert th_block["tight_interval_max_width"] == DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    assert th_block["wide_interval_min_width"] == DEFAULT_WIDE_INTERVAL_MIN_WIDTH


def test_run_auto_honours_cell_intervals_override() -> None:
    # Production-wiring seam: caller supplies (rung, codec, verdict,
    # width) per cell. A wide interval forces escalation even though
    # the smoke default verdict is GOSPEL.
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264", "libx265"),
        smoke=True,
        meta_override=SourceMeta(
            height=1080,
            width=1920,
            duration_s=120.0,
            shot_variance=0.05,
        ),
        cell_intervals=[
            (1080, "libx264", "GOSPEL", 7.0),  # wide -> FORCE
            (1080, "libx265", "FALL_BACK", 0.5),  # tight -> SKIP
        ],
    )
    decisions = {
        (e["rung"], e["codec"]): e["decision"]
        for e in plan.metadata["confidence_aware_escalations"]
    }
    assert decisions[(1080, "libx264")] == ConfidenceDecision.FORCE_ESCALATION.value
    assert decisions[(1080, "libx265")] == ConfidenceDecision.SKIP_ESCALATION.value


def test_run_auto_honours_per_corpus_threshold_override() -> None:
    # Same cell width (3.0) should land on different decisions
    # depending on whether the corpus override widens or narrows the
    # band. Test both configurations.
    narrow = ConfidenceThresholds(tight_interval_max_width=1.0, wide_interval_min_width=2.5)
    wide = ConfidenceThresholds(tight_interval_max_width=4.0, wide_interval_min_width=8.0)
    cell_intervals = [(1080, "libx264", "FALL_BACK", 3.0)]
    plan_narrow = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        cell_intervals=cell_intervals,
        confidence_thresholds=narrow,
    )
    plan_wide = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        cell_intervals=cell_intervals,
        confidence_thresholds=wide,
    )
    narrow_decisions = [e["decision"] for e in plan_narrow.metadata["confidence_aware_escalations"]]
    wide_decisions = [e["decision"] for e in plan_wide.metadata["confidence_aware_escalations"]]
    assert narrow_decisions == [ConfidenceDecision.FORCE_ESCALATION.value]
    assert wide_decisions == [ConfidenceDecision.SKIP_ESCALATION.value]


def test_run_auto_json_round_trip_includes_confidence_block() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
    )
    payload = json.loads(emit_plan_json(plan))
    assert "confidence_aware_escalations" in payload["metadata"]
    assert "confidence_thresholds" in payload["metadata"]
    # Each cell carries the per-cell decision so JSON consumers don't
    # need to cross-reference the metadata array index.
    for cell in payload["cells"]:
        assert "confidence_decision" in cell
        assert "interval_width" in cell


def test_run_auto_missing_cell_interval_falls_back_to_recommend() -> None:
    # Caller supplied cell_intervals for one cell but not the other;
    # the missing one degrades to NaN width which yields
    # RECOMMEND_ESCALATION.
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264", "libx265"),
        smoke=True,
        cell_intervals=[(1080, "libx264", "FALL_BACK", 0.5)],
    )
    decisions = {
        (e["rung"], e["codec"]): e["decision"]
        for e in plan.metadata["confidence_aware_escalations"]
    }
    assert decisions[(1080, "libx264")] == ConfidenceDecision.SKIP_ESCALATION.value
    assert decisions[(1080, "libx265")] == ConfidenceDecision.RECOMMEND_ESCALATION.value
