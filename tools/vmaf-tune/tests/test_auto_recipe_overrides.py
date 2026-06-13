# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for the F.4 per-content-type recipe overrides (ADR-0325).

The recipe table at ``vmaftune.auto._CONTENT_RECIPE_TABLE`` maps four
named content classes (``animation``, ``screen_content``,
``live_action_hdr``, ``ugc``) to override dicts that fire *before* the
F.2 short-circuits. The default class fires no override.

The tests assert:

* Every named class triggers its recipe and the documented override
  keys land in ``plan.metadata.recipe_overrides``.
* The default class fires no override; ``recipe_applied == "default"``.
* Recipes are read-only — mutating a returned recipe never leaks into
  the next ``run_auto`` call.
* The JSON output records ``metadata.recipe_applied`` in the canonical
  set ``{"animation", "screen_content", "live_action_hdr", "ugc",
  "default"}``.
* ``target_vmaf_offset`` shifts the *predictor*'s effective target
  VMAF only — the input ``target_vmaf`` (gate that ships models)
  stays untouched. Per memory ``feedback_no_test_weakening``.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune.auto import (  # noqa: E402
    _CONTENT_RECIPE_TABLE,
    DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
    RECIPE_CLASS_ANIMATION,
    RECIPE_CLASS_DEFAULT,
    RECIPE_CLASS_LIVE_ACTION_HDR,
    RECIPE_CLASS_SCREEN_CONTENT,
    RECIPE_CLASS_UGC,
    ConfidenceDecision,
    ConfidenceThresholds,
    PlanState,
    SourceMeta,
    _apply_recipe_override,
    emit_plan_json,
    get_recipe_for_class,
    run_auto,
)

_ALL_CLASSES = (
    RECIPE_CLASS_DEFAULT,
    RECIPE_CLASS_ANIMATION,
    RECIPE_CLASS_SCREEN_CONTENT,
    RECIPE_CLASS_LIVE_ACTION_HDR,
    RECIPE_CLASS_UGC,
)


def _meta(content_class: str = "live_action", **overrides) -> SourceMeta:
    base = {
        "height": 1080,
        "width": 1920,
        "is_hdr": False,
        "content_class": content_class,
        "duration_s": 60.0,
        "shot_variance": 0.05,
        "sample_clip_seconds": 0.0,
    }
    base.update(overrides)
    return SourceMeta(**base)


def _state() -> PlanState:
    return PlanState(
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
    )


# ---------------------------------------------------------------------------
# Recipe lookup primitives.
# ---------------------------------------------------------------------------


def test_recipe_table_covers_all_named_classes() -> None:
    # The four named classes plus the default key all live in the table.
    assert set(_CONTENT_RECIPE_TABLE) == set(_ALL_CLASSES)


def test_get_recipe_for_default_class_is_empty() -> None:
    assert get_recipe_for_class(RECIPE_CLASS_DEFAULT) == {}


def test_get_recipe_for_animation_has_documented_keys() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    # The ADR-0325 §F.4 spec mandates these four documented keys.
    assert "tight_interval_max_width" in recipe
    assert recipe["force_single_rung"] is True
    assert recipe["saliency_intensity"] == "aggressive"
    assert "target_vmaf_offset" in recipe


def test_get_recipe_for_screen_content_sets_saliency_very_aggressive() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_SCREEN_CONTENT)
    assert recipe["saliency_intensity"] == "very_aggressive"


def test_get_recipe_for_live_action_hdr_narrows_tight_interval() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_LIVE_ACTION_HDR)
    # HDR recipe tightens the F.3 conformal-tight gate below the
    # SDR default of 2.0.
    assert float(recipe["tight_interval_max_width"]) < 2.0


def test_get_recipe_for_ugc_widens_tight_interval() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_UGC)
    # UGC recipe widens the F.3 conformal-tight gate above the
    # SDR default of 2.0 (UGC has more variance).
    assert float(recipe["tight_interval_max_width"]) > 2.0


def test_get_recipe_unknown_class_falls_back_to_default() -> None:
    assert get_recipe_for_class("photographic") == {}
    assert get_recipe_for_class("") == {}


# ---------------------------------------------------------------------------
# Read-only invariant — mutations don't leak across calls.
# ---------------------------------------------------------------------------


def test_recipe_returns_fresh_dict_each_call() -> None:
    a = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    b = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    assert a is not b
    a["target_vmaf_offset"] = 999.0
    a["new_key"] = "leaked"
    fresh = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    assert fresh["target_vmaf_offset"] != 999.0
    assert "new_key" not in fresh


def test_apply_recipe_override_returns_fresh_recipe_per_call() -> None:
    meta = _meta(content_class=RECIPE_CLASS_ANIMATION)
    base_thresholds = ConfidenceThresholds()
    cls_a, recipe_a, _ = _apply_recipe_override(meta, _state(), base_thresholds)
    cls_b, recipe_b, _ = _apply_recipe_override(meta, _state(), base_thresholds)
    assert cls_a == cls_b == RECIPE_CLASS_ANIMATION
    assert recipe_a is not recipe_b
    recipe_a["target_vmaf_offset"] = -50.0
    _, recipe_c, _ = _apply_recipe_override(meta, _state(), base_thresholds)
    assert recipe_c["target_vmaf_offset"] != -50.0


# ---------------------------------------------------------------------------
# Per-class trigger semantics.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "content_class,expected",
    [
        ("animation", RECIPE_CLASS_ANIMATION),
        ("screen_content", RECIPE_CLASS_SCREEN_CONTENT),
        ("live_action_hdr", RECIPE_CLASS_LIVE_ACTION_HDR),
        ("ugc", RECIPE_CLASS_UGC),
    ],
)
def test_apply_recipe_override_routes_named_classes(content_class: str, expected: str) -> None:
    meta = _meta(content_class=content_class)
    cls, recipe, _ = _apply_recipe_override(meta, _state(), ConfidenceThresholds())
    assert cls == expected
    if expected == RECIPE_CLASS_DEFAULT:
        assert recipe == {}
    else:
        assert recipe  # non-empty for any named class


def test_apply_recipe_override_default_for_unknown_class() -> None:
    meta = _meta(content_class="photographic")
    cls, recipe, _ = _apply_recipe_override(meta, _state(), ConfidenceThresholds())
    assert cls == RECIPE_CLASS_DEFAULT
    assert recipe == {}


def test_apply_recipe_override_promotes_hdr_to_live_action_hdr() -> None:
    # Per ADR-0300: an HDR source with a generic ``live_action`` label
    # is auto-promoted to the HDR recipe.
    meta = _meta(content_class="live_action", is_hdr=True)
    cls, recipe, _ = _apply_recipe_override(meta, _state(), ConfidenceThresholds())
    assert cls == RECIPE_CLASS_LIVE_ACTION_HDR
    assert recipe  # HDR recipe non-empty


def test_apply_recipe_override_animation_hdr_keeps_animation() -> None:
    # An ``animation`` HDR source keeps its animation recipe (the named
    # class wins over the HDR auto-promotion).
    meta = _meta(content_class=RECIPE_CLASS_ANIMATION, is_hdr=True)
    cls, _, _ = _apply_recipe_override(meta, _state(), ConfidenceThresholds())
    assert cls == RECIPE_CLASS_ANIMATION


# ---------------------------------------------------------------------------
# Threshold narrowing — recipe never widens the production-flip gate.
# ---------------------------------------------------------------------------


def test_apply_recipe_narrows_tight_for_hdr() -> None:
    base = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
    meta = _meta(content_class=RECIPE_CLASS_LIVE_ACTION_HDR)
    _, _, eff = _apply_recipe_override(meta, _state(), base)
    assert eff.tight_interval_max_width < base.tight_interval_max_width
    # The wide gate is preserved verbatim.
    assert eff.wide_interval_min_width == base.wide_interval_min_width


def test_apply_recipe_widens_tight_for_ugc_but_clamps_to_wide() -> None:
    base = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
    meta = _meta(content_class=RECIPE_CLASS_UGC)
    _, _, eff = _apply_recipe_override(meta, _state(), base)
    # UGC widens the tight gate but stays <= the wide gate so the
    # ConfidenceThresholds invariant survives.
    assert eff.tight_interval_max_width >= base.tight_interval_max_width
    assert eff.tight_interval_max_width <= base.wide_interval_min_width


def test_apply_recipe_default_class_preserves_thresholds_verbatim() -> None:
    base = ConfidenceThresholds(tight_interval_max_width=2.0, wide_interval_min_width=5.0)
    meta = _meta(content_class="live_action")  # default route
    cls, recipe, eff = _apply_recipe_override(meta, _state(), base)
    assert cls == RECIPE_CLASS_DEFAULT
    assert recipe == {}
    assert eff.tight_interval_max_width == base.tight_interval_max_width
    assert eff.wide_interval_min_width == base.wide_interval_min_width


def test_run_auto_uses_recipe_thresholds_for_confidence_decisions() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_LIVE_ACTION_HDR)
    recipe_tight = float(recipe["tight_interval_max_width"])
    assert recipe_tight < DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    interval_width = (recipe_tight + DEFAULT_TIGHT_INTERVAL_MAX_WIDTH) / 2.0
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=_meta(content_class=RECIPE_CLASS_LIVE_ACTION_HDR, is_hdr=True),
        cell_intervals=[(1080, "libx264", "FALL_BACK", interval_width)],
    )
    thresholds = plan.metadata["confidence_thresholds"]
    assert thresholds["tight_interval_max_width"] == recipe_tight
    assert str(thresholds["source"]).startswith(f"recipe:{RECIPE_CLASS_LIVE_ACTION_HDR}/")
    escalation = plan.metadata["confidence_aware_escalations"][0]
    assert escalation["decision"] == ConfidenceDecision.RECOMMEND_ESCALATION.value


# ---------------------------------------------------------------------------
# Driver integration — recipe surfaces in JSON metadata.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "content_class,expected",
    [
        ("animation", RECIPE_CLASS_ANIMATION),
        ("screen_content", RECIPE_CLASS_SCREEN_CONTENT),
        ("live_action_hdr", RECIPE_CLASS_LIVE_ACTION_HDR),
        ("ugc", RECIPE_CLASS_UGC),
        ("live_action", RECIPE_CLASS_DEFAULT),
        ("photographic", RECIPE_CLASS_DEFAULT),
    ],
)
def test_run_auto_records_recipe_applied_in_metadata(content_class: str, expected: str) -> None:
    meta = _meta(content_class=content_class)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert plan.metadata["recipe_applied"] == expected


def test_run_auto_recipe_applied_is_in_canonical_set() -> None:
    canonical = {
        RECIPE_CLASS_ANIMATION,
        RECIPE_CLASS_SCREEN_CONTENT,
        RECIPE_CLASS_LIVE_ACTION_HDR,
        RECIPE_CLASS_UGC,
        RECIPE_CLASS_DEFAULT,
    }
    for cls in (
        "animation",
        "screen_content",
        "live_action_hdr",
        "ugc",
        "live_action",
        "photographic",
        "",
    ):
        plan = run_auto(
            src=Path("/dev/null"),
            target_vmaf=93.0,
            max_budget_kbps=5000.0,
            allow_codecs=("libx264",),
            smoke=True,
            meta_override=_meta(content_class=cls),
        )
        assert plan.metadata["recipe_applied"] in canonical


def test_run_auto_recipe_applied_round_trips_through_emit_plan_json() -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=_meta(content_class=RECIPE_CLASS_ANIMATION),
    )
    rendered = emit_plan_json(plan)
    payload = json.loads(rendered)
    assert payload["metadata"]["recipe_applied"] == RECIPE_CLASS_ANIMATION
    assert payload["metadata"]["recipe_overrides"]["force_single_rung"] is True


# ---------------------------------------------------------------------------
# Animation: force_single_rung on >= 2160p sources.
# ---------------------------------------------------------------------------


def test_run_auto_animation_forces_single_rung_at_4k() -> None:
    # The bare-F.2 short-circuit #1 would not fire (height >= 2160).
    # The animation recipe's ``force_single_rung`` arms it anyway.
    meta = _meta(content_class=RECIPE_CLASS_ANIMATION, height=2160, width=3840)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert "ladder-single-rung" in plan.metadata["short_circuits"]


def test_run_auto_default_class_does_not_force_single_rung_at_4k() -> None:
    # Without an animation recipe a 4K source goes through the multi-rung
    # path; short-circuit #1 does not fire.
    meta = _meta(content_class="live_action", height=2160, width=3840)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert "ladder-single-rung" not in plan.metadata["short_circuits"]


# ---------------------------------------------------------------------------
# target_vmaf_offset: predictor-only, never the production-flip gate.
# ---------------------------------------------------------------------------


def test_target_vmaf_offset_never_modifies_input_target() -> None:
    # The input ``target_vmaf`` is the gate that ships models; the
    # recipe MUST NOT widen it. We assert that the metadata still
    # records the input target verbatim, and the recipe-driven offset
    # only lands in ``effective_predictor_target_vmaf``.
    meta = _meta(content_class=RECIPE_CLASS_ANIMATION)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    # Input target preserved verbatim.
    assert plan.metadata["target_vmaf"] == 93.0
    # Predictor's effective target shifted by the animation recipe.
    assert plan.metadata["effective_predictor_target_vmaf"] != plan.metadata["target_vmaf"]
    assert plan.metadata["effective_predictor_target_vmaf"] == 93.0 + float(
        get_recipe_for_class(RECIPE_CLASS_ANIMATION)["target_vmaf_offset"]
    )


def test_default_class_effective_target_equals_input_target() -> None:
    meta = _meta(content_class="live_action")
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    assert plan.metadata["target_vmaf"] == 93.0
    assert plan.metadata["effective_predictor_target_vmaf"] == 93.0


# ---------------------------------------------------------------------------
# Recipe ordering — fires before F.2 short-circuits.
# ---------------------------------------------------------------------------


def test_recipe_fires_before_short_circuits() -> None:
    # A 4K animation source: without F.4 the ladder-single-rung gate
    # would NOT fire (height >= 2160). With F.4, the recipe arms
    # ``force_single_rung`` and the ladder-single-rung gate fires
    # *because* the recipe ran first. This asserts the ordering.
    meta = _meta(content_class=RECIPE_CLASS_ANIMATION, height=2160, width=3840)
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=20000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=meta,
    )
    fired = plan.metadata["short_circuits"]
    # Ladder fired despite height >= 2160 → recipe ran before F.2.
    assert "ladder-single-rung" in fired


# ---------------------------------------------------------------------------
# Saliency intensity surfacing.
# ---------------------------------------------------------------------------


@pytest.mark.parametrize(
    "content_class,expected_intensity",
    [
        (RECIPE_CLASS_ANIMATION, "aggressive"),
        (RECIPE_CLASS_SCREEN_CONTENT, "very_aggressive"),
        (RECIPE_CLASS_LIVE_ACTION_HDR, "default"),
        (RECIPE_CLASS_UGC, "default"),
        ("live_action", "default"),
    ],
)
def test_saliency_intensity_surfaces_per_recipe(
    content_class: str, expected_intensity: str
) -> None:
    plan = run_auto(
        src=Path("/dev/null"),
        target_vmaf=93.0,
        max_budget_kbps=5000.0,
        allow_codecs=("libx264",),
        smoke=True,
        meta_override=_meta(content_class=content_class),
    )
    for cell in plan.cells:
        assert cell["saliency_intensity"] == expected_intensity
