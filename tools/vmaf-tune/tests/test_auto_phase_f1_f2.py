# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the three additional F.1/F.2 short-circuit predicates.

Short-circuits #8 (low-complexity), #9 (baseline-meets-target), and #10
(no-two-pass) are pure functions of (meta, plan_state). Each predicate is
exercised at its boundary conditions; the driver-level smoke test asserts
the new short-circuits are recorded in ``plan.metadata.short_circuits``
when the trigger conditions are met.
"""

from __future__ import annotations

import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.auto import (  # noqa: E402
    LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS,
    SHORT_CIRCUIT_PREDICATES,
    PlanState,
    ShortCircuit,
    SourceMeta,
    _should_short_circuit_baseline_meets_target,
    _should_short_circuit_low_complexity,
    _should_short_circuit_no_two_pass,
    evaluate_short_circuits,
    run_auto,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _meta(**overrides) -> SourceMeta:
    base = {
        "height": 1080,
        "width": 1920,
        "is_hdr": False,
        "content_class": "live_action",
        "duration_s": 60.0,
        "shot_variance": 0.05,
        "sample_clip_seconds": 0.0,
        "complexity_score": 0.0,
        "baseline_vmaf": 0.0,
    }
    base.update(overrides)
    return SourceMeta(**base)


def _state(**overrides) -> PlanState:
    base = {
        "target_vmaf": 93.0,
        "max_budget_kbps": 5000.0,
        "allow_codecs": ("libx264",),
        "user_pinned_codec": None,
        "predictor_verdict": None,
        "adapter_supports_two_pass": None,
    }
    base.update(overrides)
    return PlanState(**base)


# ---------------------------------------------------------------------------
# Short-circuit #8 — low-complexity source.
# ---------------------------------------------------------------------------


def test_sc_low_complexity_fires_below_threshold() -> None:
    meta = _meta(complexity_score=LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS - 1.0)
    assert _should_short_circuit_low_complexity(meta, _state())


def test_sc_low_complexity_does_not_fire_at_threshold() -> None:
    meta = _meta(complexity_score=LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS)
    assert not _should_short_circuit_low_complexity(meta, _state())


def test_sc_low_complexity_does_not_fire_above_threshold() -> None:
    meta = _meta(complexity_score=LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS + 100.0)
    assert not _should_short_circuit_low_complexity(meta, _state())


def test_sc_low_complexity_does_not_fire_when_score_is_zero() -> None:
    assert not _should_short_circuit_low_complexity(_meta(complexity_score=0.0), _state())


def test_sc_low_complexity_does_not_fire_when_score_is_negative() -> None:
    assert not _should_short_circuit_low_complexity(_meta(complexity_score=-50.0), _state())


def test_sc_low_complexity_does_not_fire_when_score_is_nan() -> None:
    assert not _should_short_circuit_low_complexity(_meta(complexity_score=float("nan")), _state())


@pytest.mark.parametrize("score", [1.0, 50.0, 100.0, 199.9])
def test_sc_low_complexity_fires_on_sub_threshold_scores(score: float) -> None:
    assert _should_short_circuit_low_complexity(_meta(complexity_score=score), _state())


# ---------------------------------------------------------------------------
# Short-circuit #9 — baseline encode already meets target.
# ---------------------------------------------------------------------------


def test_sc_baseline_meets_target_fires_when_baseline_ge_target() -> None:
    assert _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=94.0), _state(target_vmaf=93.0)
    )


def test_sc_baseline_meets_target_fires_when_baseline_equals_target() -> None:
    assert _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=93.0), _state(target_vmaf=93.0)
    )


def test_sc_baseline_meets_target_does_not_fire_when_baseline_lt_target() -> None:
    assert not _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=91.5), _state(target_vmaf=93.0)
    )


def test_sc_baseline_meets_target_does_not_fire_when_baseline_is_zero() -> None:
    assert not _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=0.0), _state(target_vmaf=93.0)
    )


def test_sc_baseline_meets_target_does_not_fire_when_baseline_is_nan() -> None:
    assert not _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=float("nan")), _state(target_vmaf=93.0)
    )


def test_sc_baseline_meets_target_does_not_fire_when_baseline_is_negative() -> None:
    assert not _should_short_circuit_baseline_meets_target(
        _meta(baseline_vmaf=-1.0), _state(target_vmaf=93.0)
    )


# ---------------------------------------------------------------------------
# Short-circuit #10 — codec adapter does not support two-pass.
# ---------------------------------------------------------------------------


def test_sc_no_two_pass_fires_when_adapter_flag_is_false() -> None:
    assert _should_short_circuit_no_two_pass(_meta(), _state(adapter_supports_two_pass=False))


def test_sc_no_two_pass_does_not_fire_when_adapter_flag_is_true() -> None:
    assert not _should_short_circuit_no_two_pass(_meta(), _state(adapter_supports_two_pass=True))


def test_sc_no_two_pass_does_not_fire_when_adapter_flag_is_none() -> None:
    assert not _should_short_circuit_no_two_pass(_meta(), _state(adapter_supports_two_pass=None))


# ---------------------------------------------------------------------------
# Ordering
# ---------------------------------------------------------------------------


def test_new_short_circuits_appended_after_original_seven() -> None:
    all_scs = [sc for sc, _ in SHORT_CIRCUIT_PREDICATES]
    assert all_scs[:7] == [
        ShortCircuit.LADDER_SINGLE_RUNG,
        ShortCircuit.CODEC_PINNED,
        ShortCircuit.PREDICTOR_GOSPEL,
        ShortCircuit.SKIP_SALIENCY,
        ShortCircuit.SDR_SKIP,
        ShortCircuit.SAMPLE_CLIP_PROPAGATE,
        ShortCircuit.SKIP_PER_SHOT,
    ]
    assert all_scs[7] is ShortCircuit.LOW_COMPLEXITY
    assert all_scs[8] is ShortCircuit.BASELINE_MEETS_TARGET
    assert all_scs[9] is ShortCircuit.NO_TWO_PASS


def test_short_circuit_predicates_length_is_ten() -> None:
    assert len(SHORT_CIRCUIT_PREDICATES) == 10


# ---------------------------------------------------------------------------
# evaluate_short_circuits fires the new predicates together.
# ---------------------------------------------------------------------------


def test_evaluate_fires_all_three_new_predicates_together() -> None:
    meta = _meta(complexity_score=10.0, baseline_vmaf=99.0)
    state = _state(target_vmaf=93.0, allow_codecs=("libx264",), adapter_supports_two_pass=False)
    fired = evaluate_short_circuits(meta, state)
    assert ShortCircuit.LOW_COMPLEXITY.value in fired
    assert ShortCircuit.BASELINE_MEETS_TARGET.value in fired
    assert ShortCircuit.NO_TWO_PASS.value in fired


def test_evaluate_does_not_fire_new_predicates_when_conditions_not_met() -> None:
    meta = _meta(complexity_score=500.0, baseline_vmaf=80.0)
    state = _state(target_vmaf=93.0, allow_codecs=("libx264",), adapter_supports_two_pass=True)
    fired = evaluate_short_circuits(meta, state)
    assert ShortCircuit.LOW_COMPLEXITY.value not in fired
    assert ShortCircuit.BASELINE_MEETS_TARGET.value not in fired
    assert ShortCircuit.NO_TWO_PASS.value not in fired


# ---------------------------------------------------------------------------
# Driver-level smoke tests.
# ---------------------------------------------------------------------------


def test_run_auto_smoke_records_low_complexity_short_circuit() -> None:
    meta = SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=60.0,
        shot_variance=0.05,
        complexity_score=50.0,
        baseline_vmaf=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert ShortCircuit.LOW_COMPLEXITY.value in plan.metadata["short_circuits"]


def test_run_auto_smoke_records_baseline_meets_target_short_circuit() -> None:
    meta = SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=60.0,
        shot_variance=0.05,
        complexity_score=0.0,
        baseline_vmaf=96.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert ShortCircuit.BASELINE_MEETS_TARGET.value in plan.metadata["short_circuits"]


def test_run_auto_smoke_does_not_record_no_two_pass_for_x264() -> None:
    # libx264 declares supports_two_pass=True; predicate must NOT fire.
    meta = SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=60.0,
        shot_variance=0.05,
        complexity_score=0.0,
        baseline_vmaf=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert ShortCircuit.NO_TWO_PASS.value not in plan.metadata["short_circuits"]


def test_run_auto_smoke_does_not_record_no_two_pass_for_x265() -> None:
    # libx265 declares supports_two_pass=True; predicate must NOT fire.
    meta = SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=60.0,
        shot_variance=0.05,
        complexity_score=0.0,
        baseline_vmaf=0.0,
    )
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx265",),
        smoke=True,
        meta_override=meta,
    )
    assert ShortCircuit.NO_TWO_PASS.value not in plan.metadata["short_circuits"]


def test_run_auto_new_short_circuits_do_not_break_existing_f2_behaviour() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
    )
    fired = plan.metadata["short_circuits"]
    assert "ladder-single-rung" in fired
    assert "codec-pinned" in fired
    assert "predictor-gospel" in fired
    assert "skip-saliency" in fired
    assert "sdr-skip" in fired
    assert "skip-per-shot" in fired
