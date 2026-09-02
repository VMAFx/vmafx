# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""``vmaf-tune auto`` — Phase F adaptive recipe-aware tuning entry point.

Phase F composes the per-phase subcommands (``corpus``, ``recommend``,
``fast``, ``predict``, ``tune-per-shot``, ``recommend-saliency``,
``ladder``, ``compare``) plus the orthogonal modes (HDR, sample-clip,
resolution-aware) into a single deterministic decision tree. The
sequential composition (F.1) walks the tree top-to-bottom and runs every
stage; the seven short-circuits (F.2, this module) skip stages whose
output is determined by metadata alone.

Decision tree (per :doc:`docs/adr/0364-vmaf-tune-phase-f-auto.md`):

.. code-block:: text

   auto(src, target_vmaf, max_budget_kbps, allow_codecs):
       meta = probe(src); is_hdr = detect_hdr(meta)            # ADR-0300
       rungs = [meta.resolution] if meta.height < 2160         # ADR-0289
               else ladder.candidate_rungs(meta)
       codecs = (allow_codecs if len==1
                 else [user_pin] if user_pinned_codec
                 else compare.shortlist(allow_codecs, meta))
       plan = []
       for rung, codec in (rungs x codecs):
           v = predict.crf_for_target(rung, codec, target_vmaf, meta)
           if v.verdict == FALL_BACK:
               v = recommend.coarse_to_fine(rung, codec, target_vmaf)
           plan.append((rung, codec, v))
       if duration > 5min and shot_variance(src) > 0.15:        # Phase D gate
           plan = [tune_per_shot.refine(p) for p in plan]
       if meta.content_class in {animation, screen_content}:    # saliency gate
           plan = [recommend_saliency.maybe_apply(p) for p in plan]
       winner = pick_pareto(plan, target_vmaf, max_budget_kbps)
       return realise(winner, hdr=is_hdr)

The seven short-circuit predicates live in this module as standalone
helpers (``_should_short_circuit_<N>``) so each one is unit-testable in
isolation. Each predicate returns ``True`` when the corresponding stage
can be skipped; the main driver records the firing predicate names in
``plan.metadata.short_circuits`` for post-hoc speedup analysis.

F.3 ships per-cell confidence-aware fallbacks (escalation to
``recommend.coarse_to_fine`` driven by the conformal interval width
from :class:`vmaftune.predictor.Predictor.predict_vmaf_with_uncertainty`,
ADR-0279). F.4 ships per-content-type recipe overrides
(:func:`_apply_recipe_override`, recipes for ``animation``,
``screen_content``, ``live_action_hdr``, and ``ugc``) — the recipe
fires *before* the F.2 short-circuits evaluate so a recipe can flip
``force_single_rung`` and have the ladder stage honour it. Non-smoke
runs probe source geometry, duration, and HDR signaling through
ffprobe-backed helpers and use the predictor path for per-cell CRF,
VMAF, and bitrate estimates until the later realise/encode step lands;
``--smoke`` keeps the same planner deterministic without ffmpeg or ONNX.

See also:

* :mod:`vmaftune.fast` — the ``fast`` subcommand. Phase F's
  short-circuits do **not** shadow ``fast``'s behaviour; ``fast`` is a
  different operator surface (proxy + Bayesian over a single codec) and
  Phase F's auto driver never invokes it from inside the tree.
* :mod:`vmaftune.compare` — codec shortlist when ``--allow-codecs`` has
  more than one entry.
* :mod:`vmaftune.ladder` — multi-rung ABR ladder for >= 2160p sources.
"""

from __future__ import annotations

import dataclasses
import enum
import json
import logging
import math
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .hdr import HdrInfo
    from .predictor import ShotFeatures

_LOG = logging.getLogger(__name__)


# Phase D gate thresholds (per ADR-0364 short-circuit #7). The 5-min /
# 0.15-shot-variance pair is a placeholder; F.3 fits these from a real
# corpus once Phase F has emitted enough labelled compositions to make
# the fit statistically defensible. Until then, the placeholders keep
# the gate conservative — short low-variance content skips per-shot
# tuning, everything else gets it.
PHASE_D_DURATION_GATE_S: float = 300.0  # 5 minutes
PHASE_D_SHOT_VARIANCE_GATE: float = 0.15

# 2160p (4K) is the resolution gate above which the ladder is
# multi-rung (per ADR-0289 / ADR-0295). Below it, the auto driver
# encodes only the source rung — no need to evaluate 720p / 1080p
# rungs of a 1080p source.
LADDER_MULTI_RUNG_HEIGHT: int = 2160

# Content classes that benefit from saliency-aware ROI tuning per
# ADR-0293. Photographic / live-action content does not — saliency
# weights the centre of the frame and live-action subjects already
# get foveal weighting from VMAF's perceptual model.
SALIENCY_CONTENT_CLASSES: frozenset[str] = frozenset({"animation", "screen_content"})


# ---------------------------------------------------------------------------
# F.3 confidence-aware fallback thresholds.
#
# F.2 treats the predictor's verdict as a binary GOSPEL / FALL_BACK gate.
# F.3 makes the gate continuous by consulting the conformal interval
# half-width returned by :meth:`Predictor.predict_vmaf_with_uncertainty`
# (ADR-0279). The two thresholds below carve the half-width axis into
# three regions:
#
#   * width <= ``DEFAULT_TIGHT_INTERVAL_MAX_WIDTH`` → predictor is
#     confident; trust the point estimate even if the native verdict
#     was nominally FALL_BACK (the verdict was wrong about *which*
#     direction the residual leans, but the certainty signal
#     dominates).
#   * width >= ``DEFAULT_WIDE_INTERVAL_MIN_WIDTH`` → predictor is
#     uncertain; force escalation to ``recommend.coarse_to_fine`` even
#     if the native verdict was GOSPEL.
#   * tight < width < wide → fall back to the native verdict (F.2
#     behaviour).
#
# The defaults (2.0 and 5.0 VMAF) are documented in Research-0067 and
# act as an emergency floor when no corpus-derived sidecar is shipped
# with the calibration. The production thresholds come from a JSON
# sidecar produced by the calibration pipeline shipped in #488 — keys
# ``tight_interval_max_width`` and ``wide_interval_min_width``. The
# loader in :func:`load_confidence_thresholds` honours per-corpus
# overrides and emits a one-line warning when no sidecar is found.
DEFAULT_TIGHT_INTERVAL_MAX_WIDTH: float = 2.0
DEFAULT_WIDE_INTERVAL_MIN_WIDTH: float = 5.0


# ---------------------------------------------------------------------------
# F.4 per-content-type recipe overrides (ADR-0325 §F.4).
#
# When an upstream classifier (e.g. TransNet V2 shot histograms in
# ``tools/vmaf-tune/src/vmaftune/per_shot.py::detect_shots`` plus the
# fork-local content-class heuristics) tags a source as ``animation``,
# ``screen_content``, ``live_action_hdr``, or ``ugc``, the auto driver
# applies a small dict of override keys *before* the F.2 short-circuits
# evaluate. The override dict is applied additively — any override key
# the recipe doesn't set keeps the driver's default behaviour.
#
# The four override keys consumed by the driver are:
#
# * ``tight_interval_max_width`` (float) — narrows / widens the F.3
#   conformal-tight gate. Animation: tighter (predictor is consistent
#   on flat colour fields). Live-action HDR: tighter still (any wide
#   interval is suspect for HDR — see ADR-0300). UGC: wider (UGC has
#   more variance; predictor uncertainty is expected baseline).
# * ``force_single_rung`` (bool) — short-circuit #1 stays armed even
#   on >= 2160p sources. Animation only: a single-rung ladder + tight
#   compression makes more sense than a 5-rung sweep on stylised art
#   that compresses uniformly.
# * ``saliency_intensity`` (str) — passed through to the saliency stage
#   when it isn't skipped. ``aggressive`` for animation,
#   ``very_aggressive`` for screen-content (high QP-offset on
#   background, near-lossless on text regions). ``default`` keeps the
#   ADR-0293 baseline.
# * ``target_vmaf_offset`` (float) — additive offset applied to the
#   *predictor's* target VMAF so the planner aims slightly higher /
#   lower; the production-flip gate that ships models is **not**
#   shifted by this value (per ``feedback_no_test_weakening``). The
#   recipe override only relaxes / tightens what the predictor aims
#   for, never the gate that decides whether a model can ship.
#
# Every threshold cited below is provisional and tagged for empirical
# calibration in F.5 — see ADR-0325 §"Phase F.5 backlog" once F.4
# emits enough labelled recipe applications to fit them. The current
# values are documented placeholders, not measured outcomes; the
# only-evidence-cited footnotes are tracked in
# Research-0067 §"F.4 recipe-override placeholders".
_RECIPE_KEYS: frozenset[str] = frozenset(
    {
        "tight_interval_max_width",
        "force_single_rung",
        "saliency_intensity",
        "target_vmaf_offset",
    }
)


# Sentinel content-class strings recorded in
# ``plan.metadata.recipe_applied``. ``DEFAULT`` is the no-recipe path
# (fires when ``meta.content_class`` doesn't match any other recipe);
# the four named classes correspond to the recipes documented in
# Research-0067.
RECIPE_CLASS_DEFAULT: str = "default"
RECIPE_CLASS_ANIMATION: str = "animation"
RECIPE_CLASS_SCREEN_CONTENT: str = "screen_content"
RECIPE_CLASS_LIVE_ACTION_HDR: str = "live_action_hdr"
RECIPE_CLASS_UGC: str = "ugc"


def _empty_recipe() -> dict[str, object]:
    """Return a fresh empty override dict (default class)."""
    return {}


# F.4 placeholder constants. Used as the fallback when the F.5
# calibrated JSON (``ai/data/phase_f_recipes_calibrated.json``) is
# missing or malformed. Each value is annotated with the F.4
# rationale; F.5 fits these from a real corpus and the loader below
# replaces them at module import. Per memory ``feedback_no_guessing``
# the calibrated values are sourced from
# ``ai/scripts/calibrate_phase_f_recipes.py``; the fallback constants
# are kept verbatim so callers that ship without the JSON file (e.g.
# minimal wheel installs, smoke tests) still get the documented F.4
# behaviour.
_F4_PLACEHOLDER_RECIPES: dict[str, dict[str, object]] = {
    RECIPE_CLASS_ANIMATION: {
        "tight_interval_max_width": 1.5,
        "force_single_rung": True,
        "saliency_intensity": "aggressive",
        "target_vmaf_offset": 2.0,
    },
    RECIPE_CLASS_SCREEN_CONTENT: {
        "saliency_intensity": "very_aggressive",
        "target_vmaf_offset": 1.0,
    },
    RECIPE_CLASS_LIVE_ACTION_HDR: {
        "tight_interval_max_width": 1.2,
        "target_vmaf_offset": 0.0,
    },
    RECIPE_CLASS_UGC: {
        "tight_interval_max_width": 3.0,
        "target_vmaf_offset": -1.0,
    },
}


# Path to the calibrated-recipes JSON, relative to the repo root.
# Resolved at module import; if the file is missing or malformed the
# F.4 placeholder constants above remain in force (graceful
# degradation per ADR-0325 §F.5 status update).
_CALIBRATED_RECIPES_FILENAME: str = "ai/data/phase_f_recipes_calibrated.json"


def _find_calibrated_recipes_path() -> Path | None:
    """Walk upward from this file looking for the calibrated JSON.

    Returns ``None`` if the JSON cannot be located. The walk stops at
    the filesystem root or when ``ai/data/phase_f_recipes_calibrated.json``
    is found relative to a parent directory. This decouples the loader
    from the install layout — both source-tree checkouts and editable
    installs resolve correctly.
    """
    here = Path(__file__).resolve()
    for parent in here.parents:
        candidate = parent / _CALIBRATED_RECIPES_FILENAME
        if candidate.is_file():
            return candidate
    return None


def _load_calibrated_recipes() -> dict[str, dict[str, object]]:
    """Load the F.5-calibrated overrides, falling back to F.4 placeholders.

    Returns a dict keyed by content-class name (``animation``,
    ``screen_content``, ``live_action_hdr``, ``ugc``) holding the
    override values to merge into the per-class factory. Unknown
    classes in the JSON are ignored silently; missing classes fall
    back to the F.4 placeholder for that class.

    The JSON schema (per
    ``ai/scripts/calibrate_phase_f_recipes.py``):

    .. code-block:: json

        {
          "metadata": { ... },
          "recipes": {
            "<class>": { "<key>": <value>, "_provenance": {...} }
          }
        }

    The ``_provenance`` sub-dicts are stripped before merging so they
    never leak into ``plan.metadata.recipe_overrides``.
    """
    path = _find_calibrated_recipes_path()
    if path is None:
        _LOG.warning(
            "no calibrated recipes JSON found; using F.4 placeholder recipes "
            "(documented-placeholder values, not measured outcomes). "
            "Run 'vmaf-tune auto --calibrate' or supply a recipes JSON to "
            "suppress this warning.",
        )
        return {cls: dict(rec) for cls, rec in _F4_PLACEHOLDER_RECIPES.items()}
    try:
        with path.open("rt", encoding="utf-8") as fh:
            payload = json.load(fh)
    except (OSError, json.JSONDecodeError) as exc:
        _LOG.warning(
            "failed to read %s (%s); falling back to F.4 placeholders",
            path,
            exc,
        )
        return {cls: dict(rec) for cls, rec in _F4_PLACEHOLDER_RECIPES.items()}

    raw_recipes = payload.get("recipes")
    if not isinstance(raw_recipes, dict):
        _LOG.warning(
            "%s missing 'recipes' object; falling back to F.4 placeholders",
            path,
        )
        return {cls: dict(rec) for cls, rec in _F4_PLACEHOLDER_RECIPES.items()}

    merged: dict[str, dict[str, object]] = {
        cls: dict(rec) for cls, rec in _F4_PLACEHOLDER_RECIPES.items()
    }
    for cls, rec in raw_recipes.items():
        if cls not in merged or not isinstance(rec, dict):
            continue
        clean: dict[str, object] = {}
        for key, value in rec.items():
            if key.startswith("_"):
                continue
            if key in _RECIPE_KEYS:
                clean[key] = value
        if clean:
            merged[cls] = clean
    return merged


# Resolved at module import — a single load, snapshotted into the
# per-class factories below. Reloading at runtime requires
# ``importlib.reload(vmaftune.auto)``; this matches the F.4 read-only
# invariant exercised by ``test_auto_recipe_overrides``.
_CALIBRATED_RECIPES: dict[str, dict[str, object]] = _load_calibrated_recipes()


def _animation_recipe() -> dict[str, object]:
    """Recipe for ``content_class == "animation"``.

    Animation compresses uniformly on flat colour fields — a single-rung
    ladder is plenty, the predictor's residuals are tighter than on
    live-action, and saliency benefits from being aggressive on cel-line
    edges.

    Values are sourced from
    ``ai/data/phase_f_recipes_calibrated.json`` (F.5 calibration).
    The fallback constants in ``_F4_PLACEHOLDER_RECIPES`` apply when
    the JSON is missing or malformed.
    """
    return dict(_CALIBRATED_RECIPES[RECIPE_CLASS_ANIMATION])


def _screen_content_recipe() -> dict[str, object]:
    """Recipe for ``content_class == "screen_content"``.

    Screen content (UI captures, slideshow recordings) splits the frame
    into low-entropy background + high-detail text/icon regions; the
    saliency-aware stage benefits from a very aggressive intensity that
    raises QP on the background while keeping text near-lossless.

    Values from ``ai/data/phase_f_recipes_calibrated.json`` (F.5).
    """
    return dict(_CALIBRATED_RECIPES[RECIPE_CLASS_SCREEN_CONTENT])


def _live_action_hdr_recipe() -> dict[str, object]:
    """Recipe for ``content_class == "live_action_hdr"``.

    HDR live-action shows wide tonal swings; per ADR-0300 the HDR
    pipeline already runs, but the F.3 conformal-tight gate is narrowed
    here because a wide predictor interval on HDR is more suspect than
    on SDR (the predictor was largely trained on SDR — see ADR-0279).

    Values from ``ai/data/phase_f_recipes_calibrated.json`` (F.5).
    """
    return dict(_CALIBRATED_RECIPES[RECIPE_CLASS_LIVE_ACTION_HDR])


def _ugc_recipe() -> dict[str, object]:
    """Recipe for ``content_class == "ugc"``.

    User-generated content carries higher upstream-encode noise,
    inconsistent grading, and resolution mismatches; predictor
    uncertainty is the baseline. Widening the F.3 tight gate avoids
    over-flagging UGC cells as "needs escalation" simply because the
    interval is wider than a Netflix-grade reference.

    Values from ``ai/data/phase_f_recipes_calibrated.json`` (F.5).
    """
    return dict(_CALIBRATED_RECIPES[RECIPE_CLASS_UGC])


# Module-level recipe table. Each value is a *factory* (not a literal
# dict) so the override returned to the driver is always a fresh copy
# — mutations from one ``run_auto`` call cannot leak into the next.
# Tests assert this read-only invariant explicitly.
_CONTENT_RECIPE_TABLE: dict[str, Callable[[], dict[str, object]]] = {
    RECIPE_CLASS_ANIMATION: _animation_recipe,
    RECIPE_CLASS_SCREEN_CONTENT: _screen_content_recipe,
    RECIPE_CLASS_LIVE_ACTION_HDR: _live_action_hdr_recipe,
    RECIPE_CLASS_UGC: _ugc_recipe,
    RECIPE_CLASS_DEFAULT: _empty_recipe,
}


def _resolve_recipe_class(meta: SourceMeta) -> str:
    """Map a :class:`SourceMeta` onto a recipe-table key.

    The classifier source is ``meta.content_class`` populated upstream
    by ``per_shot.detect_shots`` + the fork-local heuristics. The
    auto-classify-as-HDR fallback below honours ``meta.is_hdr`` so an
    operator who passes ``--content-class live_action`` on an HDR
    source still gets the HDR recipe (the HDR signal trumps a generic
    live-action label).

    Returns one of :data:`RECIPE_CLASS_DEFAULT`,
    :data:`RECIPE_CLASS_ANIMATION`, :data:`RECIPE_CLASS_SCREEN_CONTENT`,
    :data:`RECIPE_CLASS_LIVE_ACTION_HDR`, :data:`RECIPE_CLASS_UGC`.
    """
    raw = (meta.content_class or "").strip().lower()
    if raw in _CONTENT_RECIPE_TABLE and raw != RECIPE_CLASS_DEFAULT:
        # Promote a "live_action" + is_hdr=True meta to the HDR recipe;
        # otherwise the explicit content_class wins.
        if raw == RECIPE_CLASS_LIVE_ACTION_HDR:
            return RECIPE_CLASS_LIVE_ACTION_HDR
        if meta.is_hdr and raw not in {
            RECIPE_CLASS_ANIMATION,
            RECIPE_CLASS_SCREEN_CONTENT,
            RECIPE_CLASS_UGC,
        }:
            return RECIPE_CLASS_LIVE_ACTION_HDR
        return raw
    # Auto-promote: an unknown class on an HDR source still gets the
    # HDR recipe (matches ADR-0300's permissive HDR detection).
    if meta.is_hdr:
        return RECIPE_CLASS_LIVE_ACTION_HDR
    return RECIPE_CLASS_DEFAULT


def get_recipe_for_class(content_class: str) -> dict[str, object]:
    """Return a fresh override dict for the named recipe class.

    Public helper consumed by tests and by callers who want to inspect
    a recipe without constructing a full :class:`SourceMeta`. The
    returned dict is always a fresh copy — mutating it never affects
    the module-level table.

    Unknown class strings degrade to the empty default recipe.
    """
    factory = _CONTENT_RECIPE_TABLE.get(
        (content_class or "").strip().lower(),
        _empty_recipe,
    )
    recipe = factory()
    # Defence in depth — only documented keys land in the recipe.
    return {key: value for key, value in recipe.items() if key in _RECIPE_KEYS}


class ShortCircuit(enum.Enum):
    """Names of the short-circuits (per ADR-0325 §F.1/F.2).

    The string values are the canonical identifiers recorded in
    ``plan.metadata.short_circuits`` and surfaced in the JSON output.

    Short-circuits #1–#7 are the original seven from the F.1/F.2
    sequential scaffold. Short-circuits #8–#10 are the three additional
    predicates introduced by this PR: low-complexity, baseline-meets-
    target, and no-two-pass.  Adding a new short-circuit means appending
    here and to ``SHORT_CIRCUIT_PREDICATES`` — never reordering.
    """

    LADDER_SINGLE_RUNG = "ladder-single-rung"
    CODEC_PINNED = "codec-pinned"
    PREDICTOR_GOSPEL = "predictor-gospel"
    SKIP_SALIENCY = "skip-saliency"
    SDR_SKIP = "sdr-skip"
    SAMPLE_CLIP_PROPAGATE = "sample-clip-propagate"
    SKIP_PER_SHOT = "skip-per-shot"
    # F.1/F.2 additions — three short-circuits that consult the probe
    # bitrate (complexity barometer), the baseline-encode VMAF, and the
    # codec adapter's two-pass flag respectively.
    LOW_COMPLEXITY = "low-complexity"
    BASELINE_MEETS_TARGET = "baseline-meets-target"
    NO_TWO_PASS = "no-two-pass"


@dataclasses.dataclass(frozen=True)
class SourceMeta:
    """Source metadata consumed by the decision tree.

    The fields mirror the per-phase ADR contracts: ``height`` from
    ``ffprobe`` (ADR-0289), ``is_hdr`` from
    :func:`vmaftune.hdr.detect_hdr` (ADR-0300), ``content_class`` from
    the F.4 classifier (placeholder until F.4 lands; defaults to
    ``"live_action"`` so the saliency gate stays a no-op on unknown
    content), ``duration_s`` and ``shot_variance`` from the per-shot
    detector (ADR-0276 phase-d).
    """

    height: int
    width: int
    is_hdr: bool = False
    content_class: str = "live_action"
    duration_s: float = 0.0
    shot_variance: float = 0.0
    sample_clip_seconds: float = 0.0
    # F.1/F.2 additions: complexity barometer + baseline VMAF.
    # ``complexity_score`` is the probe-encode bitrate at the adapter's
    # ``probe_quality``/``probe_preset`` knobs. ``0.0`` or ``NaN`` means
    # the probe hasn't run yet; short-circuit #8 does not fire.
    # ``baseline_vmaf`` is the pooled-mean VMAF at the codec's default
    # CRF. ``0.0`` or ``NaN`` means no baseline scored; short-circuit #9
    # does not fire.
    complexity_score: float = 0.0
    baseline_vmaf: float = 0.0


def _default_hdr_info_for_auto():
    """Return conservative PQ HDR metadata for metadata-only auto runs.

    `SourceMeta` intentionally carries only the boolean `is_hdr` signal.
    Production non-smoke runs keep the richer `HdrInfo` returned by
    `detect_hdr`; tests and API callers that pass `meta_override` still
    need deterministic codec-specific dispatch, so they fall back to the
    same BT.2020/PQ tuple the old scaffold hard-coded.
    """
    from .hdr import HdrInfo

    return HdrInfo(
        transfer="pq",
        primaries="bt2020",
        matrix="bt2020nc",
        color_range="tv",
        pix_fmt="yuv420p10le",
    )


def _probe_source_duration(
    src: Path,
    *,
    ffprobe_bin: str,
    runner: Callable[..., Any],
) -> float:
    """Return source duration in seconds, or ``0.0`` when probing fails."""
    cmd = [
        ffprobe_bin,
        "-v",
        "error",
        "-show_entries",
        "format=duration",
        "-of",
        "json",
        str(src),
    ]
    try:
        completed = runner(cmd, capture_output=True, text=True, check=False)
    except (OSError, FileNotFoundError):
        return 0.0
    if int(getattr(completed, "returncode", 1)) != 0:
        return 0.0
    try:
        payload = json.loads(getattr(completed, "stdout", "") or "{}")
        return float(payload.get("format", {}).get("duration", 0.0))
    except (json.JSONDecodeError, TypeError, ValueError):
        return 0.0


def _probe_source_meta(
    src: Path,
    *,
    sample_clip_seconds: float,
    runner: Callable[..., Any] | None = None,
) -> tuple[SourceMeta, Any | None]:
    """Probe metadata needed by the non-smoke auto planner.

    The helper is the production seam for ffprobe-bound source facts:
    geometry and duration come from ffprobe, HDR signaling comes from
    :func:`vmaftune.hdr.detect_hdr`, and all failures degrade to the
    same conservative defaults the planner used before the probe path
    was extracted. Tests pass a fake ``runner`` so the production path
    is covered without depending on host ffprobe behavior.
    """
    import subprocess

    from .hdr import detect_hdr
    from .predictor_features import (
        FeatureExtractorConfig,
        _probe_video_geometry,
    )

    actual_runner = runner or subprocess.run
    cfg = FeatureExtractorConfig()
    width, height, _fps = _probe_video_geometry(src, cfg, actual_runner)
    hdr_info = detect_hdr(src, runner=actual_runner)
    duration_s = _probe_source_duration(src, ffprobe_bin=cfg.ffprobe_bin, runner=actual_runner)
    return (
        SourceMeta(
            height=height or 1080,
            width=width or 1920,
            is_hdr=hdr_info is not None,
            duration_s=duration_s,
            sample_clip_seconds=sample_clip_seconds,
        ),
        hdr_info,
    )


@dataclasses.dataclass
class PlanState:
    """Mutable state threaded through the decision tree.

    Each stage of the tree consults this state and may mutate the
    ``short_circuits`` list, the ``predictor_verdict`` field, the
    ``codecs`` list, etc. The driver returns the final state in the
    plan's ``metadata`` block so post-hoc analysis can measure which
    short-circuits fired.
    """

    target_vmaf: float
    max_budget_kbps: float
    allow_codecs: tuple[str, ...]
    user_pinned_codec: str | None = None
    predictor_verdict: str | None = None  # "GOSPEL" / "LIKELY" / "FALL_BACK"
    short_circuits: list[str] = dataclasses.field(default_factory=list)
    # F.1/F.2 addition: per-cell two-pass support flag from the codec adapter.
    # ``None`` means not yet resolved (smoke/pre-cell-loop); predicate #10
    # does not fire for ``None``.
    adapter_supports_two_pass: bool | None = None

    def fired(self, sc: ShortCircuit) -> None:
        """Record that ``sc`` fired (idempotent on repeats)."""
        if sc.value not in self.short_circuits:
            self.short_circuits.append(sc.value)


# ---------------------------------------------------------------------------
# The seven short-circuit predicates. Each returns True when the stage
# the predicate guards can be skipped. Predicates are pure functions of
# (meta, plan_state) so tests mock the inputs and assert the branch
# fires / doesn't fire without invoking the full driver.
# ---------------------------------------------------------------------------


def _should_short_circuit_1_single_rung_ladder(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #1 — single-rung ladder when ``meta.height < 2160``.

    Per ADR-0364 / ADR-0289: sub-4K sources don't need a multi-rung
    Per ADR-0325 / ADR-0289: sub-4K sources don't need a multi-rung
    ABR ladder evaluation; the source rung is the only candidate. The
    driver still runs the per-rung pipeline, just on one rung.
    """
    del plan_state  # unused — predicate depends on meta only
    return int(meta.height) < LADDER_MULTI_RUNG_HEIGHT


def _should_short_circuit_2_codec_pinned(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #2 — codec known / pinned.

    When ``--allow-codecs`` resolves to exactly one codec, the
    ``compare.shortlist`` stage adds no information — there's nothing
    to compare against. Skip directly to the per-codec sweep.
    """
    del meta  # unused — predicate depends on plan_state only
    if plan_state.user_pinned_codec is not None:
        return True
    return len(plan_state.allow_codecs) == 1


def _should_short_circuit_3_predictor_gospel(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #3 — predictor returned GOSPEL.

    Per ADR-0364 escalation rule: when ``predict.crf_for_target``
    Per ADR-0325 escalation rule: when ``predict.crf_for_target``
    returns ``GOSPEL`` (residuals within threshold across the
    validation sample), trust the predictor's CRF pick and skip the
    ``recommend.coarse_to_fine`` fallback for that cell.
    """
    del meta  # unused — predicate depends on plan_state only
    return plan_state.predictor_verdict == "GOSPEL"


def _should_short_circuit_4_skip_saliency(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #4 — photographic / non-screen-content skips saliency.

    Per ADR-0293: saliency-aware ROI tuning is gated on content class.
    ``animation`` and ``screen_content`` benefit; ``live_action`` /
    ``photographic`` does not (VMAF's perceptual model already gives
    centre-frame foveal weighting). Skip ``recommend_saliency.maybe_apply``.
    """
    del plan_state  # unused — predicate depends on meta only
    return meta.content_class not in SALIENCY_CONTENT_CLASSES


def _should_short_circuit_5_sdr_skip(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #5 — SDR source skips the HDR pipeline.

    Per ADR-0300: HDR detection is permissive; an SDR source skips the
    HDR resolution + model-selection branch. ``meta.is_hdr`` is
    populated by :func:`vmaftune.hdr.detect_hdr`; ``False`` here is
    the canonical "treat as SDR" signal.
    """
    del plan_state  # unused — predicate depends on meta only
    return not bool(meta.is_hdr)


def _should_short_circuit_6_sample_clip_propagate(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #6 — propagate user-supplied sample-clip.

    When the user passed ``--sample-clip-seconds`` (ADR-0301), the
    auto driver propagates that value to the internal sweeps rather
    than re-deciding clip length per stage. This is a propagation
    short-circuit, not a stage-skip — the flag is recorded in the
    metadata so downstream stages know to honour it verbatim.
    """
    del plan_state  # unused — predicate depends on meta only
    return float(meta.sample_clip_seconds) > 0.0


def _should_short_circuit_7_skip_per_shot(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #7 — duration / shot-variance gate.

    Per ADR-0364: skip ``tune_per_shot.refine`` when the source is
    Per ADR-0325: skip ``tune_per_shot.refine`` when the source is
    both short (< 5 min) **and** low-variance (shot variance < 0.15).
    Either condition alone is not enough — a short high-variance
    trailer benefits from per-shot, and a long low-variance lecture
    capture also benefits. The thresholds are placeholders pending
    F.3 empirical fit.
    """
    del plan_state  # unused — predicate depends on meta only
    short = float(meta.duration_s) < PHASE_D_DURATION_GATE_S
    low_variance = float(meta.shot_variance) < PHASE_D_SHOT_VARIANCE_GATE
    return short and low_variance


# ---------------------------------------------------------------------------
# F.1/F.2 addition — three new short-circuit predicates (#8, #9, #10).
# ---------------------------------------------------------------------------

# Threshold below which a source is considered "low complexity" and the
# recommend / ladder stages add no information. Expressed in kbps of the
# codec adapter's probe encode. ``0.0`` disables the gate (smoke mode, no
# probe). Placeholder pending F.3 corpus-derived fit.
LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS: float = 200.0


def _should_short_circuit_low_complexity(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #8 — low-complexity source skips recommend / ladder.

    When the probe-encode bitrate (``meta.complexity_score``) is below
    ``LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS``, the predictor's
    point estimate is already tight enough that running
    ``recommend.coarse_to_fine`` adds no meaningful improvement. A value
    of ``0.0`` or ``NaN`` means the probe hasn't run (smoke mode); the
    predicate does **not** fire so smoke runs are never gated.
    """
    del plan_state  # unused — predicate depends on meta only
    score = float(meta.complexity_score)
    if math.isnan(score) or score <= 0.0:
        return False
    return score < LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS


def _should_short_circuit_baseline_meets_target(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #9 — baseline encode already meets target VMAF.

    When ``meta.baseline_vmaf`` (the VMAF of a default-CRF encode) is
    already at or above ``plan_state.target_vmaf``, running the predictor
    sweep and ``recommend.coarse_to_fine`` is redundant. A value of
    ``0.0`` or ``NaN`` means the baseline hasn't been scored (smoke mode);
    the predicate does **not** fire.
    """
    baseline = float(meta.baseline_vmaf)
    if math.isnan(baseline) or baseline <= 0.0:
        return False
    return baseline >= float(plan_state.target_vmaf)


def _should_short_circuit_no_two_pass(meta: SourceMeta, plan_state: PlanState) -> bool:
    """Short-circuit #10 — codec adapter does not support two-pass encode.

    When the resolved codec adapter's ``supports_two_pass`` flag is
    ``False`` (per the :class:`CodecAdapter` protocol, ADR-0333), the
    two-pass calibration stage adds no information. ``None`` (unresolved,
    smoke mode or pre-cell-loop) does **not** fire so evaluation-order
    bugs don't silently suppress two-pass encodes.
    """
    del meta  # unused — predicate depends on plan_state only
    flag = plan_state.adapter_supports_two_pass
    if flag is None:
        return False
    return not bool(flag)


# Ordered tuple of (ShortCircuit, predicate). The order is the
# evaluation order in the driver and is part of the public contract:
# tests assert that an earlier-firing predicate doesn't shadow a
# later one whose result would have been different. Adding a new
# short-circuit means appending here, never reordering.
SHORT_CIRCUIT_PREDICATES: tuple[
    tuple[ShortCircuit, Callable[[SourceMeta, PlanState], bool]], ...
] = (
    (ShortCircuit.LADDER_SINGLE_RUNG, _should_short_circuit_1_single_rung_ladder),
    (ShortCircuit.CODEC_PINNED, _should_short_circuit_2_codec_pinned),
    (ShortCircuit.PREDICTOR_GOSPEL, _should_short_circuit_3_predictor_gospel),
    (ShortCircuit.SKIP_SALIENCY, _should_short_circuit_4_skip_saliency),
    (ShortCircuit.SDR_SKIP, _should_short_circuit_5_sdr_skip),
    (ShortCircuit.SAMPLE_CLIP_PROPAGATE, _should_short_circuit_6_sample_clip_propagate),
    (ShortCircuit.SKIP_PER_SHOT, _should_short_circuit_7_skip_per_shot),
    # F.1/F.2 additions — appended in canonical order.
    (ShortCircuit.LOW_COMPLEXITY, _should_short_circuit_low_complexity),
    (ShortCircuit.BASELINE_MEETS_TARGET, _should_short_circuit_baseline_meets_target),
    (ShortCircuit.NO_TWO_PASS, _should_short_circuit_no_two_pass),
)


def evaluate_short_circuits(meta: SourceMeta, plan_state: PlanState) -> list[str]:
    """Run every predicate in declaration order, recording firers.

    Returns the list of fired short-circuit names (the same list
    available on ``plan_state.short_circuits``). Pure function over
    its inputs — calling it twice on the same state yields the same
    list, and predicate order is deterministic.
    """
    for sc, predicate in SHORT_CIRCUIT_PREDICATES:
        if predicate(meta, plan_state):
            plan_state.fired(sc)
    return list(plan_state.short_circuits)


# ---------------------------------------------------------------------------
# F.1 sequential planner + F.2 short-circuit-aware driver.
# The non-smoke path probes source metadata through ffprobe/HDR helpers;
# ``--smoke`` skips those process-bound probes and uses deterministic
# synthetic metadata.
# ---------------------------------------------------------------------------


def _apply_recipe_override(
    meta: SourceMeta,
    plan_state: PlanState,
    confidence_thresholds: ConfidenceThresholds,
) -> tuple[str, dict[str, object], ConfidenceThresholds]:
    """Resolve + apply the F.4 per-content-type recipe.

    Fires **before** the F.2 short-circuits evaluate so a recipe can
    flip ``force_single_rung`` and have the ladder stage honour it.
    Returns a 3-tuple ``(recipe_class, recipe, effective_thresholds)``:

    * ``recipe_class`` — the canonical class string recorded under
      ``plan.metadata.recipe_applied``. One of
      :data:`RECIPE_CLASS_DEFAULT`, :data:`RECIPE_CLASS_ANIMATION`,
      :data:`RECIPE_CLASS_SCREEN_CONTENT`,
      :data:`RECIPE_CLASS_LIVE_ACTION_HDR`, :data:`RECIPE_CLASS_UGC`.
    * ``recipe`` — the fresh override dict (a copy, not a shared
      reference). Empty for the default class.
    * ``effective_thresholds`` — ``confidence_thresholds`` with the
      recipe's ``tight_interval_max_width`` applied (if set). The
      ``wide_interval_min_width`` is preserved verbatim — F.4 only
      tightens / loosens the *predictor*-confidence gate, not the
      hard "force escalation" wall.

    Per the no-test-weakening rule (memory
    ``feedback_no_test_weakening``), the recipe never widens the
    production-flip gate; ``target_vmaf_offset`` is an offset on the
    predictor's *target*, not on the gate that ships models.
    """
    del plan_state  # reserved for future per-cell recipes; unused at F.4
    recipe_class = _resolve_recipe_class(meta)
    recipe = get_recipe_for_class(recipe_class)
    effective_thresholds = confidence_thresholds
    tight_override = recipe.get("tight_interval_max_width")
    if tight_override is not None:
        new_tight = float(tight_override)  # type: ignore[arg-type]
        wide = float(confidence_thresholds.wide_interval_min_width)
        # Clamp so we never violate the constructor invariant
        # (tight <= wide). A recipe that asks for a tight wider than
        # the corpus-fit wide is silently capped.
        new_tight = min(new_tight, wide)
        effective_thresholds = ConfidenceThresholds(
            tight_interval_max_width=new_tight,
            wide_interval_min_width=wide,
            source=f"recipe:{recipe_class}/{confidence_thresholds.source}",
        )
    return recipe_class, recipe, effective_thresholds


@dataclasses.dataclass
class AutoPlan:
    """Result of an ``auto`` run.

    ``cells`` is one entry per ``(rung, codec)`` cell; for the smoke
    path the entries are placeholders (no real encode happened) but
    the schema is stable from F.1 onwards.
    """

    cells: list[dict]
    metadata: dict


def _predictor_features_from_meta(meta: SourceMeta) -> ShotFeatures:
    """Build predictor features from metadata-only auto inputs.

    This is the first production step past the F.1 placeholder cell:
    when a non-smoke caller has already probed the source, the auto
    driver can use the existing analytical / ONNX predictor path to
    choose a CRF without running a full coarse-to-fine sweep. Features
    that require per-frame probe logs stay at zero until the Phase F
    probe-encode capture lands; the predictor contract explicitly
    treats those as unavailable signals.
    """
    from .predictor import ShotFeatures

    fps = 30.0
    duration_s = max(float(meta.duration_s), 0.0)
    shot_frames = round(duration_s * fps) if duration_s > 0.0 else int(fps)
    pixels = max(int(meta.width), 1) * max(int(meta.height), 1)
    # Complexity score is the probe bitrate when available. If the
    # caller has not supplied it yet, seed the predictor with a
    # resolution-proportional neutral bitrate so non-smoke metadata
    # runs still produce a codec-specific CRF instead of the old fixed
    # placeholder.
    probe_bitrate = float(meta.complexity_score)
    if math.isnan(probe_bitrate) or probe_bitrate <= 0.0:
        probe_bitrate = max(500.0, pixels / 900.0)
    return ShotFeatures(
        probe_bitrate_kbps=probe_bitrate,
        probe_i_frame_avg_bytes=0.0,
        probe_p_frame_avg_bytes=0.0,
        probe_b_frame_avg_bytes=0.0,
        frame_diff_mean=max(float(meta.shot_variance), 0.0),
        shot_length_frames=max(shot_frames, 1),
        fps=fps,
        width=max(int(meta.width), 1),
        height=max(int(meta.height), 1),
    )


def _estimate_cell_bitrate_kbps(features: ShotFeatures, codec: str, crf: int) -> float:
    """Estimate bitrate for an auto cell from probe bitrate + CRF.

    The value is explicitly a predictor estimate, not a measured encode
    result. It follows the common encoder rule of thumb that six CRF/QP
    points roughly double / halve bitrate, anchored at the adapter's
    probe quality. This gives downstream planners a monotone bitrate
    estimate until the full encode/score realise step lands.
    """
    from .codec_adapters import get_adapter

    adapter = get_adapter(codec)
    probe_quality = int(getattr(adapter, "probe_quality", getattr(adapter, "quality_default", crf)))
    scale = 2.0 ** ((float(probe_quality) - float(crf)) / 6.0)
    return max(1.0, float(features.probe_bitrate_kbps) * scale)


def _finite_float(value: object) -> float | None:
    """Return ``value`` as a finite float, or ``None`` when unusable."""
    try:
        out = float(value)  # type: ignore[arg-type]
    except (TypeError, ValueError):
        return None
    if not math.isfinite(out):
        return None
    return out


def pick_auto_winner(
    cells: Sequence[dict],
    *,
    target_vmaf: float,
    max_budget_kbps: float,
) -> dict[str, object]:
    """Pick the Phase F realised cell from estimated plan rows.

    The selector is intentionally conservative:

    * prefer cells that satisfy both the quality target and bitrate budget;
    * if no cell is inside budget, keep the quality gate and minimise budget
      overage;
    * if no cell meets quality, return the closest quality miss so callers get
      a concrete next encode instead of an empty plan.

    Ties favour lower bitrate, then higher VMAF, then a higher rung, then a
    stable codec/name ordering.
    """
    scored: list[tuple[int, dict, float, float]] = []
    for index, cell in enumerate(cells):
        estimated_vmaf = _finite_float(cell.get("estimated_vmaf"))
        estimated_bitrate = _finite_float(cell.get("estimated_bitrate_kbps"))
        if estimated_vmaf is None or estimated_bitrate is None:
            continue
        scored.append((index, cell, estimated_vmaf, estimated_bitrate))

    if not scored:
        return {
            "status": "no_eligible_cells",
            "reason": "no cell carried finite estimated_vmaf and estimated_bitrate_kbps",
        }

    target = float(target_vmaf)
    budget = float(max_budget_kbps)
    passing = [item for item in scored if item[2] >= target and item[3] <= budget]
    if passing:
        status = "budget_and_quality_met"
        selected = min(
            passing,
            key=lambda item: (
                item[3],
                -item[2],
                -int(item[1].get("rung", 0)),
                str(item[1].get("codec", "")),
                item[0],
            ),
        )
    else:
        quality_only = [item for item in scored if item[2] >= target]
        if quality_only:
            status = "quality_met_budget_exceeded"
            selected = min(
                quality_only,
                key=lambda item: (
                    item[3] - budget,
                    item[3],
                    -item[2],
                    -int(item[1].get("rung", 0)),
                    str(item[1].get("codec", "")),
                    item[0],
                ),
            )
        else:
            status = "target_unmet"
            selected = max(
                scored,
                key=lambda item: (
                    item[2],
                    -item[3],
                    int(item[1].get("rung", 0)),
                    str(item[1].get("codec", "")),
                    -item[0],
                ),
            )

    index, cell, estimated_vmaf, estimated_bitrate = selected
    return {
        "status": status,
        "cell_index": index,
        "rung": int(cell.get("rung", 0)),
        "codec": str(cell.get("codec", "")),
        "crf": int(cell.get("crf", 0)),
        "estimated_vmaf": estimated_vmaf,
        "estimated_bitrate_kbps": estimated_bitrate,
        "quality_margin": estimated_vmaf - target,
        "budget_margin_kbps": budget - estimated_bitrate,
    }


def _mark_selected_cell(cells: list[dict], winner: dict[str, object]) -> None:
    """Annotate cells in-place with the winner selected by ``pick_auto_winner``."""
    selected_index = winner.get("cell_index")
    for index, cell in enumerate(cells):
        cell["selected"] = index == selected_index


def run_auto(
    *,
    src: Path,
    target_vmaf: float,
    max_budget_kbps: float,
    allow_codecs: Sequence[str],
    user_pinned_codec: str | None = None,
    sample_clip_seconds: float = 0.0,
    smoke: bool = False,
    meta_override: SourceMeta | None = None,
    confidence_thresholds: ConfidenceThresholds | None = None,
    cell_intervals: Sequence[tuple[int, str, str | None, float]] | None = None,
    probe_runner: Callable[..., Any] | None = None,
) -> AutoPlan:
    """Drive the F.1 + F.2 + F.3 decision tree.

    The non-smoke path probes source metadata from the actual source
    and uses the predictor path for per-cell CRF / bitrate / VMAF
    estimates. ``smoke=True`` skips process-bound probes and exercises
    the composition end-to-end with synthetic metadata.

    ``meta_override`` lets callers (and tests) inject a pre-built
    :class:`SourceMeta`. When ``None`` and ``smoke=True``, a synthetic
    1080p SDR live-action meta is fabricated so the smoke run is
    deterministic without touching ffprobe.

    ``confidence_thresholds`` carries the F.3 width gates. ``None``
    falls back to the documented defaults (2.0 / 5.0) — call
    :func:`load_confidence_thresholds` to honour a calibration sidecar.

    ``cell_intervals`` is the F.3 production-wiring seam: a sequence
    of ``(rung, codec, verdict, interval_width)`` tuples, one per
    cell, that the driver consumes when wiring
    :meth:`Predictor.predict_vmaf_with_uncertainty` into the
    per-cell predict step. ``None`` keeps the smoke synthesis (a
    constant tight interval per cell so the gate is exercised
    deterministically without ONNX). Any (rung, codec) cell missing
    from the sequence falls back to a NaN interval (uncalibrated)
    plus the driver's smoke verdict.

    ``probe_runner`` is the subprocess seam used by the non-smoke
    metadata probe. Production callers leave it ``None``; tests pass a
    fake runner that returns ffprobe-compatible JSON.
    """
    detected_hdr_info = None
    if not smoke and meta_override is None:
        meta_override, detected_hdr_info = _probe_source_meta(
            src,
            sample_clip_seconds=sample_clip_seconds,
            runner=probe_runner,
        )

    meta = meta_override or SourceMeta(
        height=1080,
        width=1920,
        is_hdr=False,
        content_class="live_action",
        duration_s=120.0,
        shot_variance=0.05,
        sample_clip_seconds=sample_clip_seconds,
    )
    hdr_info = detected_hdr_info if bool(meta.is_hdr) else None
    if bool(meta.is_hdr) and hdr_info is None:
        hdr_info = _default_hdr_info_for_auto()

    plan_state = PlanState(
        target_vmaf=target_vmaf,
        max_budget_kbps=max_budget_kbps,
        allow_codecs=tuple(allow_codecs),
        user_pinned_codec=user_pinned_codec,
    )

    # ------------------------------------------------------------------
    # Stage 0 — F.4 per-content-type recipe override.
    #
    # Fires *before* the F.2 short-circuits so a recipe can flip
    # `force_single_rung` and have the ladder stage honour it.
    # `effective_thresholds` carries the recipe-narrowed F.3 width gate
    # for the rest of the driver. ``recipe_applied`` lands in the JSON
    # metadata block so post-hoc analysis can audit which content class
    # drove the recipe choice. The predictor's effective target VMAF
    # is offset by ``target_vmaf_offset`` (recipe key) but the
    # production-flip gate that ships models is *not* shifted by this
    # value — see ``feedback_no_test_weakening`` memory.
    # ------------------------------------------------------------------
    base_thresholds = confidence_thresholds or ConfidenceThresholds()
    recipe_class, recipe, effective_thresholds = _apply_recipe_override(
        meta, plan_state, base_thresholds
    )
    target_vmaf_offset = float(recipe.get("target_vmaf_offset", 0.0))  # type: ignore[arg-type]
    effective_predictor_target_vmaf = float(target_vmaf) + target_vmaf_offset
    force_single_rung = bool(recipe.get("force_single_rung", False))
    saliency_intensity = str(recipe.get("saliency_intensity", "default"))

    # ------------------------------------------------------------------
    # Stage 1 — ladder rung selection (short-circuit #1).
    # ------------------------------------------------------------------
    if _should_short_circuit_1_single_rung_ladder(meta, plan_state) or force_single_rung:
        plan_state.fired(ShortCircuit.LADDER_SINGLE_RUNG)
        rungs: tuple[int, ...] = (int(meta.height),)
    else:
        # Multi-rung path — production wiring delegates to ladder.py.
        rungs = (2160, 1440, 1080, 720, 540)

    # ------------------------------------------------------------------
    # Stage 2 — codec shortlist (short-circuit #2).
    # ------------------------------------------------------------------
    if _should_short_circuit_2_codec_pinned(meta, plan_state):
        plan_state.fired(ShortCircuit.CODEC_PINNED)
        codecs: tuple[str, ...] = (user_pinned_codec,) if user_pinned_codec else tuple(allow_codecs)
    else:
        # Production wiring delegates to compare.shortlist; smoke
        # path keeps the full allow-list.
        codecs = tuple(allow_codecs)

    # ------------------------------------------------------------------
    # Stage 3 — HDR pipeline (short-circuit #5).
    # ------------------------------------------------------------------
    _hdr_codec_args: Callable[[str, HdrInfo], tuple[str, ...]] | None = None
    if _should_short_circuit_5_sdr_skip(meta, plan_state):
        plan_state.fired(ShortCircuit.SDR_SKIP)
        hdr_info = None
    else:
        from .hdr import hdr_codec_args as _hdr_codec_args

    # ------------------------------------------------------------------
    # Stage 4 — sample-clip propagation (short-circuit #6).
    # ------------------------------------------------------------------
    propagated_clip = 0.0
    if _should_short_circuit_6_sample_clip_propagate(meta, plan_state):
        plan_state.fired(ShortCircuit.SAMPLE_CLIP_PROPAGATE)
        propagated_clip = float(meta.sample_clip_seconds)

    # ------------------------------------------------------------------
    # Stage 5 — per-cell predictor + escalation (short-circuit #3 +
    # F.3 confidence-aware override).
    #
    # In smoke mode we synthesise a GOSPEL verdict so the F.2 gate
    # fires in the unit smoke run; production wiring will set the
    # verdict from predictor_validate.ValidationReport.verdict and
    # the interval width from
    # Predictor.predict_vmaf_with_uncertainty (ADR-0279).
    # ------------------------------------------------------------------
    if smoke:
        plan_state.predictor_verdict = "GOSPEL"

    # Use the recipe-narrowed thresholds for the rest of the driver.
    thresholds = effective_thresholds
    # Build a (rung, codec) -> (verdict, width) lookup from the
    # production-wiring seam. Missing cells fall back to (verdict,
    # NaN) — NaN width defers F.3 to the native verdict so the gate
    # degrades gracefully when no calibration is available.
    interval_lookup: dict[tuple[int, str], tuple[str | None, float]] = {}
    if cell_intervals is not None:
        for rung_in, codec_in, verdict_in, width_in in cell_intervals:
            interval_lookup[(int(rung_in), str(codec_in))] = (
                verdict_in,
                float(width_in),
            )

    confidence_aware_escalations: list[dict] = []
    cells: list[dict] = []
    predictor = None
    predictor_features = None
    if not smoke:
        from .predictor import Predictor

        predictor = Predictor()
        predictor_features = _predictor_features_from_meta(meta)
    for rung in rungs:
        for codec in codecs:
            cell_state = dataclasses.replace(plan_state)
            cell_state.short_circuits = list(plan_state.short_circuits)
            if _should_short_circuit_3_predictor_gospel(meta, cell_state):
                cell_state.fired(ShortCircuit.PREDICTOR_GOSPEL)
                # Carry the cell-level firing back up so the metadata
                # block records that GOSPEL fired at least once.
                if ShortCircuit.PREDICTOR_GOSPEL.value not in plan_state.short_circuits:
                    plan_state.fired(ShortCircuit.PREDICTOR_GOSPEL)

            # F.3 — consult the conformal interval to decide whether
            # the native verdict is overridden. The synthetic smoke
            # default is a tight interval (width=1.0) below the
            # tight_interval_max_width gate; production wiring
            # supplies real widths via cell_intervals.
            cell_key = (int(rung), str(codec))
            if cell_key in interval_lookup:
                cell_verdict, cell_width = interval_lookup[cell_key]
            elif cell_intervals is not None:
                # Caller opted into the production-wiring seam but
                # didn't cover this cell — degrade to NaN so the F.3
                # gate defers to the native verdict instead of
                # silently using a synthetic tight width.
                cell_verdict = plan_state.predictor_verdict
                cell_width = float("nan")
            else:
                cell_verdict = plan_state.predictor_verdict
                cell_width = 1.0 if smoke else float("nan")
            decision = _confidence_aware_escalation(cell_verdict, cell_width, thresholds)
            cell_hdr_args = ()
            if hdr_info is not None and _hdr_codec_args is not None:
                cell_hdr_args = _hdr_codec_args(str(codec), hdr_info)
            confidence_aware_escalations.append(
                {
                    "rung": int(rung),
                    "codec": str(codec),
                    "verdict": cell_verdict or "UNKNOWN",
                    "interval_width": cell_width,
                    "decision": decision.value,
                }
            )
            if predictor is not None and predictor_features is not None:
                from .predictor import pick_crf

                crf = pick_crf(
                    predictor,
                    predictor_features,
                    float(effective_predictor_target_vmaf),
                    str(codec),
                )
                estimated_vmaf = predictor.predict_vmaf(predictor_features, crf, str(codec))
                estimated_bitrate_kbps = _estimate_cell_bitrate_kbps(
                    predictor_features,
                    str(codec),
                    crf,
                )
                prediction_source = "predictor"
            else:
                crf = 23
                estimated_vmaf = float(target_vmaf)
                estimated_bitrate_kbps = float(max_budget_kbps)
                prediction_source = "smoke-placeholder"

            cells.append(
                {
                    "rung": int(rung),
                    "codec": str(codec),
                    "verdict": cell_verdict or plan_state.predictor_verdict or "UNKNOWN",
                    "crf": int(crf),
                    "estimated_vmaf": float(estimated_vmaf),
                    "estimated_bitrate_kbps": float(estimated_bitrate_kbps),
                    "hdr_args": list(cell_hdr_args),
                    "sample_clip_seconds": propagated_clip,
                    "confidence_decision": decision.value,
                    "interval_width": cell_width,
                    "effective_predictor_target_vmaf": float(effective_predictor_target_vmaf),
                    "prediction_source": prediction_source,
                    "saliency_intensity": saliency_intensity,
                }
            )

    # ------------------------------------------------------------------
    # Stage 6 — saliency gate (short-circuit #4).
    # ------------------------------------------------------------------
    if _should_short_circuit_4_skip_saliency(meta, plan_state):
        plan_state.fired(ShortCircuit.SKIP_SALIENCY)
    # else: production wiring would call recommend_saliency.maybe_apply
    # on every cell.

    # ------------------------------------------------------------------
    # Stage 7 — per-shot refinement gate (short-circuit #7).
    # ------------------------------------------------------------------
    if _should_short_circuit_7_skip_per_shot(meta, plan_state):
        plan_state.fired(ShortCircuit.SKIP_PER_SHOT)
    # else: production wiring would call tune_per_shot.refine on
    # every cell.

    # ------------------------------------------------------------------
    # Stage 8 — low-complexity source (short-circuit #8).
    # Does not fire when complexity_score is 0.0 / NaN (no probe yet).
    # ------------------------------------------------------------------
    if _should_short_circuit_low_complexity(meta, plan_state):
        plan_state.fired(ShortCircuit.LOW_COMPLEXITY)

    # ------------------------------------------------------------------
    # Stage 9 — baseline encode already meets target (short-circuit #9).
    # Does not fire when baseline_vmaf is 0.0 / NaN (no baseline yet).
    # ------------------------------------------------------------------
    if _should_short_circuit_baseline_meets_target(meta, plan_state):
        plan_state.fired(ShortCircuit.BASELINE_MEETS_TARGET)

    # ------------------------------------------------------------------
    # Stage 10 — per-cell no-two-pass gate (short-circuit #10).
    # Resolve supports_two_pass from the first codec in the list;
    # None (smoke / pre-cell) keeps the predicate dormant.
    # ------------------------------------------------------------------
    if codecs:
        try:
            from .codec_adapters import get_adapter as _get_adapter

            _first_adapter = _get_adapter(codecs[0])
            plan_state.adapter_supports_two_pass = bool(
                getattr(_first_adapter, "supports_two_pass", False)
            )
        except (KeyError, ImportError):
            plan_state.adapter_supports_two_pass = False
        if _should_short_circuit_no_two_pass(meta, plan_state):
            plan_state.fired(ShortCircuit.NO_TWO_PASS)

    winner = pick_auto_winner(
        cells,
        target_vmaf=target_vmaf,
        max_budget_kbps=max_budget_kbps,
    )
    _mark_selected_cell(cells, winner)

    metadata = {
        "src": str(src),
        "target_vmaf": float(target_vmaf),
        "max_budget_kbps": float(max_budget_kbps),
        "allow_codecs": list(allow_codecs),
        "user_pinned_codec": user_pinned_codec,
        "smoke": bool(smoke),
        "source_meta": dataclasses.asdict(meta),
        "short_circuits": list(plan_state.short_circuits),
        "confidence_aware_escalations": confidence_aware_escalations,
        "confidence_thresholds": {
            "tight_interval_max_width": thresholds.tight_interval_max_width,
            "wide_interval_min_width": thresholds.wide_interval_min_width,
            "source": thresholds.source,
        },
        "recipe_applied": recipe_class,
        "recipe_overrides": dict(recipe),
        "effective_predictor_target_vmaf": float(effective_predictor_target_vmaf),
        "winner": winner,
    }
    return AutoPlan(cells=cells, metadata=metadata)


# ---------------------------------------------------------------------------
# F.3 confidence-aware fallback policy (ADR-0364 §F.3 / ADR-0279).
# F.3 confidence-aware fallback policy (ADR-0325 §F.3 / ADR-0279).
#
# These helpers are pure functions of (verdict, interval_width,
# thresholds). The driver calls them per-(rung, codec) cell after the
# predictor returns and records the decision in
# ``plan.metadata.confidence_aware_escalations`` so post-hoc analysis
# can audit which cells were promoted / demoted by the conformal
# signal.
# ---------------------------------------------------------------------------


class ConfidenceDecision(enum.Enum):
    """Outcomes of :func:`_confidence_aware_escalation`.

    The three values map cleanly onto the F.2 gate:

    * :attr:`SKIP_ESCALATION` — predictor is confident enough that the
      native verdict's FALL_BACK signal is overridden; trust the point
      estimate and skip ``recommend.coarse_to_fine``.
    * :attr:`RECOMMEND_ESCALATION` — interval width is in the middle
      band; defer to the native verdict (this preserves the F.2
      contract — RECOMMEND_ESCALATION on a GOSPEL verdict still skips,
      RECOMMEND_ESCALATION on a FALL_BACK verdict still escalates).
    * :attr:`FORCE_ESCALATION` — predictor is uncertain enough that the
      native verdict's GOSPEL signal is overridden; escalate even if
      F.2 would have skipped.
    """

    SKIP_ESCALATION = "skip-escalation"
    RECOMMEND_ESCALATION = "recommend-escalation"
    FORCE_ESCALATION = "force-escalation"


@dataclasses.dataclass(frozen=True)
class ConfidenceThresholds:
    """Width thresholds carved from the calibration corpus.

    The two fields gate F.3's confidence-aware policy. The defaults are
    the emergency floor (Research-0067 §"Phase F decision tree"); the
    production values come from a calibration sidecar produced by the
    conformal-VQA pipeline (ADR-0279 / #488). ``source`` records where
    the values came from for the JSON metadata block.

    A valid threshold pair satisfies
    ``0 < tight_interval_max_width <= wide_interval_min_width``. The
    constructor enforces this so a malformed sidecar fails fast rather
    than silently producing nonsense decisions.
    """

    tight_interval_max_width: float = DEFAULT_TIGHT_INTERVAL_MAX_WIDTH
    wide_interval_min_width: float = DEFAULT_WIDE_INTERVAL_MIN_WIDTH
    source: str = "default"

    def __post_init__(self) -> None:
        tight = float(self.tight_interval_max_width)
        wide = float(self.wide_interval_min_width)
        if not (tight > 0.0 and wide > 0.0):
            raise ValueError(
                "ConfidenceThresholds: both widths must be positive; "
                f"got tight={tight!r}, wide={wide!r}"
            )
        if tight > wide:
            raise ValueError(
                "ConfidenceThresholds: tight_interval_max_width must be "
                f"<= wide_interval_min_width; got tight={tight!r}, "
                f"wide={wide!r}"
            )


def load_confidence_thresholds(sidecar_path: Path | None) -> ConfidenceThresholds:
    """Load corpus-derived thresholds from a calibration sidecar.

    The sidecar is the JSON file produced by the conformal-VQA
    calibration pipeline (#488). Expected schema (extra keys ignored
    so the loader survives schema growth)::

        {
          "tight_interval_max_width": 1.6,
          "wide_interval_min_width": 4.2,
          ...
        }

    On any failure path — ``None`` argument, missing file, malformed
    JSON, missing key — the loader falls back to the documented
    defaults (2.0 / 5.0) and emits a one-line WARNING. Per the
    no-test-weakening rule in CLAUDE.md, the defaults are the *floor*
    surface — they keep the gate functional but signal that the corpus
    fit hasn't landed yet.
    """
    if sidecar_path is None:
        _LOG.warning(
            "vmaf-tune auto F.3: no calibration sidecar provided; "
            "falling back to documented defaults "
            "(tight=%.1f, wide=%.1f).",
            DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
            DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
        )
        return ConfidenceThresholds()
    path = Path(sidecar_path)
    if not path.exists():
        _LOG.warning(
            "vmaf-tune auto F.3: calibration sidecar %s not found; "
            "falling back to documented defaults "
            "(tight=%.1f, wide=%.1f).",
            path,
            DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
            DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
        )
        return ConfidenceThresholds()
    try:
        doc = json.loads(path.read_text(encoding="utf-8"))
        tight = float(doc["tight_interval_max_width"])
        wide = float(doc["wide_interval_min_width"])
    except (OSError, ValueError, KeyError, TypeError) as exc:
        _LOG.warning(
            "vmaf-tune auto F.3: calibration sidecar %s unreadable (%s); "
            "falling back to documented defaults "
            "(tight=%.1f, wide=%.1f).",
            path,
            exc,
            DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
            DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
        )
        return ConfidenceThresholds()
    return ConfidenceThresholds(
        tight_interval_max_width=tight,
        wide_interval_min_width=wide,
        source=str(path),
    )


def _confidence_aware_escalation(
    verdict: str | None,
    interval_width: float,
    thresholds: ConfidenceThresholds,
) -> ConfidenceDecision:
    """Decide per-cell escalation from verdict + conformal interval.

    Pure function of its three inputs:

    * ``verdict`` — the native predictor verdict (``GOSPEL`` /
      ``LIKELY`` / ``FALL_BACK`` / ``None``). ``None`` is treated as
      "no native verdict" and degrades to the width-only branch.
    * ``interval_width`` — full conformal interval width
      (``high - low`` from :class:`vmaftune.conformal.ConformalInterval`).
      Must be non-negative; ``NaN`` (uncalibrated predictor) defers to
      the native verdict and returns
      :attr:`ConfidenceDecision.RECOMMEND_ESCALATION` (no override).
    * ``thresholds`` — :class:`ConfidenceThresholds` from
      :func:`load_confidence_thresholds`.

    Decision table::

        width <= tight                → SKIP_ESCALATION       (override)
        width >= wide                 → FORCE_ESCALATION      (override)
        tight < width < wide          → defer to verdict:
            verdict == "FALL_BACK"    → RECOMMEND_ESCALATION
            verdict in {GOSPEL,...}   → SKIP_ESCALATION
            verdict is None           → RECOMMEND_ESCALATION

    The native verdict's GOSPEL/FALL_BACK signal is honoured in the
    middle band — that preserves F.2's gate exactly when the predictor
    is neither confident nor uncertain. The override branches are the
    only places F.3 disagrees with F.2.
    """
    width = float(interval_width)
    if math.isnan(width):
        # Uncalibrated predictor — fall back to native verdict.
        if verdict == "FALL_BACK":
            return ConfidenceDecision.RECOMMEND_ESCALATION
        return ConfidenceDecision.RECOMMEND_ESCALATION
    if width < 0.0:
        raise ValueError(
            f"_confidence_aware_escalation: interval_width must be " f">= 0.0 or NaN; got {width!r}"
        )
    if width <= thresholds.tight_interval_max_width:
        return ConfidenceDecision.SKIP_ESCALATION
    if width >= thresholds.wide_interval_min_width:
        return ConfidenceDecision.FORCE_ESCALATION
    # Middle band — defer to the native verdict.
    if verdict == "FALL_BACK":
        return ConfidenceDecision.RECOMMEND_ESCALATION
    if verdict is None:
        return ConfidenceDecision.RECOMMEND_ESCALATION
    # GOSPEL / LIKELY / any other "trust the point" verdict.
    return ConfidenceDecision.SKIP_ESCALATION


def emit_plan_json(plan: AutoPlan) -> str:
    """Serialise ``plan`` to a stable JSON string.

    Schema is the public contract for downstream consumers (the MCP
    server, the CI corpus collector, post-hoc speedup analysis). Keys
    are sorted so the output is reproducible across runs.
    """
    payload = {"cells": plan.cells, "metadata": plan.metadata}
    return json.dumps(payload, indent=2, sort_keys=True)


__all__ = [
    "DEFAULT_TIGHT_INTERVAL_MAX_WIDTH",
    "DEFAULT_WIDE_INTERVAL_MIN_WIDTH",
    "LADDER_MULTI_RUNG_HEIGHT",
    "LOW_COMPLEXITY_PROBE_BITRATE_THRESHOLD_KBPS",
    "PHASE_D_DURATION_GATE_S",
    "PHASE_D_SHOT_VARIANCE_GATE",
    "RECIPE_CLASS_ANIMATION",
    "RECIPE_CLASS_DEFAULT",
    "RECIPE_CLASS_LIVE_ACTION_HDR",
    "RECIPE_CLASS_SCREEN_CONTENT",
    "RECIPE_CLASS_UGC",
    "SALIENCY_CONTENT_CLASSES",
    "SHORT_CIRCUIT_PREDICATES",
    "AutoPlan",
    "ConfidenceDecision",
    "ConfidenceThresholds",
    "PlanState",
    "ShortCircuit",
    "SourceMeta",
    "_apply_recipe_override",
    "_confidence_aware_escalation",
    "_probe_source_duration",
    "_probe_source_meta",
    "_should_short_circuit_1_single_rung_ladder",
    "_should_short_circuit_2_codec_pinned",
    "_should_short_circuit_3_predictor_gospel",
    "_should_short_circuit_4_skip_saliency",
    "_should_short_circuit_5_sdr_skip",
    "_should_short_circuit_6_sample_clip_propagate",
    "_should_short_circuit_7_skip_per_shot",
    "_should_short_circuit_baseline_meets_target",
    "_should_short_circuit_low_complexity",
    "_should_short_circuit_no_two_pass",
    "emit_plan_json",
    "evaluate_short_circuits",
    "get_recipe_for_class",
    "load_confidence_thresholds",
    "pick_auto_winner",
    "run_auto",
]
