# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Phase F.5 tests — calibrated-recipe loader (ADR-0325).

These tests exercise the new ``_load_calibrated_recipes`` /
``_find_calibrated_recipes_path`` helpers in :mod:`vmaftune.auto`:

* The shipped ``ai/data/phase_f_recipes_calibrated.json`` file is
  picked up by ``_find_calibrated_recipes_path`` and parsed into a
  recipe table that satisfies every F.4 routing invariant
  (the F.4 tests in :mod:`test_auto_recipe_overrides` re-run
  against the calibrated values via the existing parametrisations).

* When the JSON is missing, malformed, or has the wrong top-level
  shape, the loader degrades gracefully to the F.4 placeholder
  constants (``_F4_PLACEHOLDER_RECIPES``).

* Every override key in the JSON is restricted to the
  ``_RECIPE_KEYS`` allow-list; provenance sub-dicts (``_provenance``)
  never leak into a recipe.

* Per memory ``feedback_no_test_weakening``: the calibration JSON
  cannot widen the production-flip gate. The driver-level invariants
  in :mod:`test_auto_recipe_overrides` (``test_apply_recipe_widens_
  tight_for_ugc_but_clamps_to_wide``) are re-asserted here against
  the calibrated values to make the rule explicit.
"""

from __future__ import annotations

import json
import sys
from pathlib import Path

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

import vmaftune.auto as auto  # noqa: E402
from vmaftune.auto import (  # noqa: E402
    _CONTENT_RECIPE_TABLE,
    _F4_PLACEHOLDER_RECIPES,
    _RECIPE_KEYS,
    RECIPE_CLASS_ANIMATION,
    RECIPE_CLASS_LIVE_ACTION_HDR,
    RECIPE_CLASS_SCREEN_CONTENT,
    RECIPE_CLASS_UGC,
    _find_calibrated_recipes_path,
    _load_calibrated_recipes,
    get_recipe_for_class,
)

# ---------------------------------------------------------------------------
# Path resolution.
# ---------------------------------------------------------------------------


def test_calibrated_json_is_discoverable() -> None:
    """The shipped JSON is found by walking up from auto.py."""
    path = _find_calibrated_recipes_path()
    assert path is not None, "ai/data/phase_f_recipes_calibrated.json missing"
    assert path.is_file()
    assert path.name == "phase_f_recipes_calibrated.json"


def test_calibrated_json_is_well_formed() -> None:
    """The shipped JSON parses cleanly and carries the documented schema."""
    path = _find_calibrated_recipes_path()
    assert path is not None
    with path.open("rt", encoding="utf-8") as fh:
        payload = json.load(fh)
    assert payload["metadata"]["phase"] == "F.5"
    assert payload["metadata"]["adr"] == "ADR-0325"
    assert "recipes" in payload
    for cls in (
        RECIPE_CLASS_ANIMATION,
        RECIPE_CLASS_SCREEN_CONTENT,
        RECIPE_CLASS_LIVE_ACTION_HDR,
        RECIPE_CLASS_UGC,
    ):
        assert cls in payload["recipes"], f"missing class {cls}"
        assert "_provenance" in payload["recipes"][cls]


# ---------------------------------------------------------------------------
# Loader fall-through.
# ---------------------------------------------------------------------------


def test_loader_strips_provenance_keys(tmp_path, monkeypatch) -> None:
    """Provenance sub-dicts never leak into the recipe."""
    fake_json = tmp_path / "ai" / "data" / "phase_f_recipes_calibrated.json"
    fake_json.parent.mkdir(parents=True)
    payload = {
        "metadata": {"phase": "F.5"},
        "recipes": {
            RECIPE_CLASS_UGC: {
                "tight_interval_max_width": 2.7,
                "target_vmaf_offset": -0.5,
                "_provenance": {"source": "corpus"},
            },
        },
    }
    fake_json.write_text(json.dumps(payload), encoding="utf-8")
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: fake_json)
    loaded = _load_calibrated_recipes()
    assert "_provenance" not in loaded[RECIPE_CLASS_UGC]
    assert loaded[RECIPE_CLASS_UGC]["tight_interval_max_width"] == 2.7


def test_loader_filters_unknown_keys(tmp_path, monkeypatch) -> None:
    """Keys outside ``_RECIPE_KEYS`` are dropped silently."""
    fake_json = tmp_path / "calibrated.json"
    payload = {
        "metadata": {"phase": "F.5"},
        "recipes": {
            RECIPE_CLASS_ANIMATION: {
                "tight_interval_max_width": 1.7,
                "force_single_rung": True,
                "saliency_intensity": "aggressive",
                "target_vmaf_offset": 2.5,
                "rogue_key": "should_be_dropped",
            },
        },
    }
    fake_json.write_text(json.dumps(payload), encoding="utf-8")
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: fake_json)
    loaded = _load_calibrated_recipes()
    assert "rogue_key" not in loaded[RECIPE_CLASS_ANIMATION]
    for key in loaded[RECIPE_CLASS_ANIMATION]:
        assert key in _RECIPE_KEYS


def test_loader_falls_back_when_json_missing(monkeypatch) -> None:
    """A missing JSON file degrades to F.4 placeholder constants."""
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: None)
    loaded = _load_calibrated_recipes()
    for cls, placeholder in _F4_PLACEHOLDER_RECIPES.items():
        assert loaded[cls] == placeholder


def test_loader_falls_back_when_json_malformed(tmp_path, monkeypatch) -> None:
    """A non-JSON payload at the path degrades to F.4 placeholders."""
    bad = tmp_path / "garbage.json"
    bad.write_text("{not valid json", encoding="utf-8")
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: bad)
    loaded = _load_calibrated_recipes()
    for cls, placeholder in _F4_PLACEHOLDER_RECIPES.items():
        assert loaded[cls] == placeholder


def test_loader_falls_back_when_recipes_missing(tmp_path, monkeypatch) -> None:
    """A JSON with no ``recipes`` object degrades to F.4 placeholders."""
    bad = tmp_path / "noschema.json"
    bad.write_text(json.dumps({"metadata": {"phase": "F.5"}}), encoding="utf-8")
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: bad)
    loaded = _load_calibrated_recipes()
    for cls, placeholder in _F4_PLACEHOLDER_RECIPES.items():
        assert loaded[cls] == placeholder


def test_loader_per_class_partial_falls_back(tmp_path, monkeypatch) -> None:
    """Classes absent from the JSON keep their F.4 placeholder values."""
    fake_json = tmp_path / "partial.json"
    # Only animation calibrated; the other three fall back.
    payload = {
        "metadata": {"phase": "F.5"},
        "recipes": {
            RECIPE_CLASS_ANIMATION: {
                "tight_interval_max_width": 9.99,
                "force_single_rung": True,
                "saliency_intensity": "aggressive",
                "target_vmaf_offset": 9.99,
            },
        },
    }
    fake_json.write_text(json.dumps(payload), encoding="utf-8")
    monkeypatch.setattr(auto, "_find_calibrated_recipes_path", lambda: fake_json)
    loaded = _load_calibrated_recipes()
    assert loaded[RECIPE_CLASS_ANIMATION]["target_vmaf_offset"] == 9.99
    assert loaded[RECIPE_CLASS_UGC] == _F4_PLACEHOLDER_RECIPES[RECIPE_CLASS_UGC]


# ---------------------------------------------------------------------------
# F.4 routing invariants re-asserted against calibrated values.
# ---------------------------------------------------------------------------


def test_calibrated_animation_keeps_documented_keys() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    assert "tight_interval_max_width" in recipe
    assert recipe["force_single_rung"] is True
    assert recipe["saliency_intensity"] == "aggressive"
    assert "target_vmaf_offset" in recipe


def test_calibrated_screen_content_keeps_very_aggressive_saliency() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_SCREEN_CONTENT)
    assert recipe["saliency_intensity"] == "very_aggressive"


def test_calibrated_live_action_hdr_narrows_tight_interval() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_LIVE_ACTION_HDR)
    assert float(recipe["tight_interval_max_width"]) < 2.0


def test_calibrated_ugc_widens_tight_interval() -> None:
    recipe = get_recipe_for_class(RECIPE_CLASS_UGC)
    assert float(recipe["tight_interval_max_width"]) > 2.0


# ---------------------------------------------------------------------------
# Production-flip gate invariant — calibration MUST NOT widen the gate
# beyond the wide-interval ceiling. Per memory ``feedback_no_test_weakening``.
# ---------------------------------------------------------------------------


def test_calibrated_ugc_width_below_wide_gate_ceiling() -> None:
    """Calibrated UGC width never exceeds 5.0 (default wide ceiling)."""
    recipe = get_recipe_for_class(RECIPE_CLASS_UGC)
    width = float(recipe["tight_interval_max_width"])
    # 5.0 = ConfidenceThresholds.wide_interval_min_width default; the
    # F.3 invariant is that ``tight <= wide``. Calibration must not
    # break this. If a future re-calibration exceeds 5.0 the
    # ConfidenceThresholds default needs to move first via a separate
    # ADR — calibration alone is not allowed to widen the gate.
    assert width <= 5.0, (
        f"calibrated UGC tight_interval_max_width ({width}) exceeds "
        "the default wide-interval ceiling (5.0); calibration must "
        "not widen the production gate per feedback_no_test_weakening"
    )


def test_calibrated_offsets_within_documented_envelope() -> None:
    """Every calibrated ``target_vmaf_offset`` stays in [-2.0, +2.0]."""
    # The F.4 docstring envelope; the calibration script clamps to
    # this range (see ai/scripts/calibrate_phase_f_recipes.py
    # ``_ugc_target_vmaf_offset``). Re-assert here so a future
    # re-calibration that drops the clamp gets caught at test time.
    for cls in (
        RECIPE_CLASS_ANIMATION,
        RECIPE_CLASS_SCREEN_CONTENT,
        RECIPE_CLASS_LIVE_ACTION_HDR,
        RECIPE_CLASS_UGC,
    ):
        recipe = get_recipe_for_class(cls)
        offset = float(recipe.get("target_vmaf_offset", 0.0))
        assert -2.0 <= offset <= 2.0, f"{cls} target_vmaf_offset {offset} outside [-2.0, +2.0]"


# ---------------------------------------------------------------------------
# Read-only invariant survives the calibration loader.
# ---------------------------------------------------------------------------


def test_calibrated_recipe_returns_fresh_dict_each_call() -> None:
    a = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    b = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    assert a is not b
    a["target_vmaf_offset"] = 999.0
    fresh = get_recipe_for_class(RECIPE_CLASS_ANIMATION)
    assert fresh["target_vmaf_offset"] != 999.0


def test_recipe_table_unchanged_by_calibration() -> None:
    """The five-class table key set is invariant across calibration runs."""
    assert set(_CONTENT_RECIPE_TABLE) == {
        "animation",
        "screen_content",
        "live_action_hdr",
        "ugc",
        "default",
    }
