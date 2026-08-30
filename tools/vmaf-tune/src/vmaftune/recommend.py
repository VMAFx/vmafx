# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Predicate-driven CRF recommendation over a corpus.

The Phase A corpus already produces ``(preset, crf, bitrate_kbps,
vmaf_score)`` tuples. ``recommend`` re-uses those rows and applies a
user-supplied predicate:

- ``--target-vmaf T`` — return the row with the *smallest* CRF whose
  ``vmaf_score >= T``. Falls back to the row with the highest VMAF if
  no row clears the bar.
- ``--target-bitrate B`` — return the row whose ``bitrate_kbps`` is
  closest to ``B`` (absolute distance, ties broken by smaller CRF).

Mutually exclusive — exactly one target must be specified.

The orchestration is deliberately thin: the corpus loader either
consumes a pre-existing JSONL (``--from-corpus``) or builds one on the
fly with the same machinery as the ``corpus`` subcommand. The
predicate evaluation is independent of how the rows were obtained.

Implements Buckets #4 (target-bitrate) and #5 (target-vmaf) from the
capability audit (Research-0061).

Uncertainty-aware extension (ADR-0279, this PR)
------------------------------------------------

The conformal-VQA prediction surface in :mod:`vmaftune.conformal`
(PR #488) wraps the predictor's point VMAF estimate in a
``(point, low, high)`` interval whose width carries the predictor's
local confidence. When the caller supplies that interval per row
(via :class:`UncertaintyAwareRequest`) the search loop becomes:

* **Tight interval** (``width <= tight_max``) — the predictor is
  confident; ``pick_target_vmaf_with_uncertainty`` short-circuits
  the search as soon as the *first* row whose ``low`` clears the
  target is observed. The "interval-aware search cost" is
  :math:`O(k)` instead of the :math:`O(n)` full scan, where ``k``
  is the index of the first sufficiently-confident row.
* **Wide interval** (``width >= wide_min``) — the predictor is
  uncertain; the search refuses to short-circuit on any single row
  and instead falls back to the full point-estimate scan with the
  returned :class:`RecommendResult` flagged ``predicate=...
  (UNCERTAIN)`` so downstream tooling knows the result was picked
  under high uncertainty.
* **Middle band** — defer to the native non-uncertainty recipe
  (the existing :func:`pick_target_vmaf` semantics, unchanged).

Math: a row "definitely clears the target" iff its conformal lower
bound ``low >= target`` (the entire 1-alpha interval lies above
the bar). A row "definitely misses" iff ``high < target`` (the
whole interval lies below). The decision rule above promotes the
first definitely-clearing row only when the interval is tight
enough that the conservative ``low`` is a faithful proxy for the
truth (Lei et al. 2018 Theorem 2.2 marginal-coverage bound).

Per :mod:`vmaftune.uncertainty` documentation, the uncertainty
recipe **only** affects search cost / which row gets picked from
an *equivalence class of qualifying rows*; it does **not** widen
the production-flip gate that lives in
:mod:`vmaftune.predictor_validate`.
"""

from __future__ import annotations

import dataclasses
import json
import math
from collections.abc import Iterable, Iterator, Sequence
from pathlib import Path

from .uncertainty import (
    ConfidenceDecision,
    ConfidenceThresholds,
    classify_interval,
    interval_excludes_target,
)


@dataclasses.dataclass(frozen=True)
class RecommendRequest:
    """Predicate description.

    Exactly one of ``target_vmaf`` / ``target_bitrate_kbps`` must be set.
    Validation lives in :func:`validate_request` so the CLI layer and
    library callers share the same exit-code semantics.
    """

    target_vmaf: float | None = None
    target_bitrate_kbps: float | None = None
    encoder: str | None = None
    preset: str | None = None


@dataclasses.dataclass(frozen=True)
class RecommendResult:
    """Single winning row + the predicate that picked it."""

    row: dict
    predicate: str
    margin: float
    """Predicate-specific distance from the target.

    For ``target-vmaf``: ``vmaf_score - target`` (positive = clears the bar).
    For ``target-bitrate``: ``bitrate_kbps - target`` (signed).
    """


def validate_request(req: RecommendRequest) -> None:
    """Enforce mutually-exclusive target. Raises :class:`ValueError`."""
    has_vmaf = req.target_vmaf is not None
    has_bitrate = req.target_bitrate_kbps is not None
    if has_vmaf and has_bitrate:
        raise ValueError(
            "--target-vmaf and --target-bitrate are mutually exclusive; " "specify exactly one"
        )
    if not (has_vmaf or has_bitrate):
        raise ValueError("missing target: pass --target-vmaf or --target-bitrate")


def _filter_rows(rows: Iterable[dict], req: RecommendRequest) -> list[dict]:
    """Drop rows that fail the encoder/preset filter or have NaN VMAF."""
    out: list[dict] = []
    for row in rows:
        if req.encoder is not None and row.get("encoder") != req.encoder:
            continue
        if req.preset is not None and row.get("preset") != req.preset:
            continue
        if int(row.get("exit_status", 0)) != 0:
            continue
        v = row.get("vmaf_score")
        if v is None:
            continue
        try:
            fv = float(v)
        except (TypeError, ValueError):
            continue
        if not math.isfinite(fv):
            continue
        out.append(row)
    return out


def pick_target_vmaf(rows: Sequence[dict], target: float) -> RecommendResult:
    """Smallest CRF whose VMAF clears ``target``.

    Falls back to the row with the highest VMAF if none clears the bar
    — the user gets the closest miss rather than an empty result.
    """
    if not rows:
        raise ValueError("no eligible rows to evaluate (after filtering)")
    clearing = [r for r in rows if float(r["vmaf_score"]) >= target]
    if clearing:
        winner = min(clearing, key=lambda r: (int(r["crf"]), -float(r["vmaf_score"])))
        return RecommendResult(
            row=winner,
            predicate=f"target_vmaf>={target}",
            margin=float(winner["vmaf_score"]) - target,
        )
    # No row clears the bar — return the row that comes closest from below.
    winner = max(rows, key=lambda r: float(r["vmaf_score"]))
    return RecommendResult(
        row=winner,
        predicate=f"target_vmaf>={target} (UNMET)",
        margin=float(winner["vmaf_score"]) - target,
    )


def pick_target_bitrate(rows: Sequence[dict], target_kbps: float) -> RecommendResult:
    """Row whose bitrate is closest to ``target_kbps`` (absolute distance).

    Ties on distance go to the lower CRF (higher quality), which matches
    the producer intent "give me the best quality fitting under the
    bitrate cap" when multiple cells land on the same point.
    """
    if not rows:
        raise ValueError("no eligible rows to evaluate (after filtering)")
    winner = min(
        rows,
        key=lambda r: (
            abs(float(r["bitrate_kbps"]) - target_kbps),
            int(r["crf"]),
        ),
    )
    return RecommendResult(
        row=winner,
        predicate=f"|bitrate-{target_kbps}|->min",
        margin=float(winner["bitrate_kbps"]) - target_kbps,
    )


def recommend(rows: Iterable[dict], req: RecommendRequest) -> RecommendResult:
    """Top-level dispatcher: validate, filter, apply the predicate."""
    validate_request(req)
    eligible = _filter_rows(rows, req)
    if req.target_vmaf is not None:
        return pick_target_vmaf(eligible, req.target_vmaf)
    assert req.target_bitrate_kbps is not None  # proven by validate_request
    return pick_target_bitrate(eligible, req.target_bitrate_kbps)


def load_corpus_jsonl(path: Path) -> Iterator[dict]:
    """Stream rows from a JSONL file written by the ``corpus`` subcommand."""
    with path.open("r", encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if not line:
                continue
            yield json.loads(line)


def format_result(result: RecommendResult) -> str:
    """Human-readable single-line summary for the CLI."""
    row = result.row
    return (
        f"encoder={row.get('encoder')} preset={row.get('preset')} "
        f"crf={row.get('crf')} vmaf={float(row['vmaf_score']):.3f} "
        f"bitrate_kbps={float(row['bitrate_kbps']):.2f} "
        f"predicate={result.predicate} margin={result.margin:+.3f}"
    )


# ---------------------------------------------------------------------------
# Uncertainty-aware extension (ADR-0279) — interval-aware CRF search.
# ---------------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class UncertaintyAwareRequest:
    """Predicate description with per-row conformal intervals.

    ``thresholds`` carries the corpus-derived ``(tight, wide)`` gate
    pair — defaults match :class:`vmaftune.uncertainty.ConfidenceThresholds`
    (2.0 / 5.0 VMAF, sourced from Research-0067 / PR #495).
    """

    target_vmaf: float
    thresholds: ConfidenceThresholds = dataclasses.field(default_factory=ConfidenceThresholds)
    encoder: str | None = None
    preset: str | None = None
    sample_uncertainty: dict[int, tuple[float, float, float]] | None = None
    """Per-CRF override of ``(point, low, high)``.

    Keys are integer CRF values; values are the conformal triplet for
    that CRF. When supplied the search bypasses each row's embedded
    ``vmaf_interval`` block and uses the override — useful for tests
    and for callers that produce intervals out-of-band (e.g. the
    deep-ensemble + conformal pipeline in
    :mod:`vmaftune.conformal`).
    """


@dataclasses.dataclass(frozen=True)
class UncertaintyRecommendResult:
    """Winning row + the band that drove the decision.

    Adds ``decision`` and ``visited`` to the base
    :class:`RecommendResult` so callers can audit which short-circuit
    fired and how many rows were examined before it did.
    """

    row: dict
    predicate: str
    margin: float
    decision: ConfidenceDecision
    visited: int
    """Number of rows the search examined before terminating.

    With a tight interval the search short-circuits at the first
    definitely-clearing row, so ``visited`` is typically much less
    than ``len(rows)``. With a wide interval the search is forced
    through the full scan and ``visited == len(rows)``.
    """


def _row_interval(row: dict, req: UncertaintyAwareRequest) -> tuple[float, float, float]:
    """Resolve ``(point, low, high)`` for one row.

    Resolution order:

    1. ``req.sample_uncertainty[crf]`` (per-call override).
    2. ``row["vmaf_interval"]`` block (embedded in the corpus row).
    3. Degenerate ``(point, point, point)`` (uncalibrated — no
       interval information; the recipe falls back to point semantics).

    Returns NaN ``low`` / ``high`` when the row carries no
    ``vmaf_score`` — the caller treats those as "skip this row".
    """
    point_raw = row.get("vmaf_score")
    if point_raw is None:
        return (float("nan"), float("nan"), float("nan"))
    point = float(point_raw)
    if req.sample_uncertainty is not None:
        crf_key = int(row.get("crf", -1))
        override = req.sample_uncertainty.get(crf_key)
        if override is not None:
            return (float(override[0]), float(override[1]), float(override[2]))
    iv = row.get("vmaf_interval")
    if isinstance(iv, dict):
        low_raw = iv.get("low")
        high_raw = iv.get("high")
        if low_raw is not None and high_raw is not None:
            return (point, float(low_raw), float(high_raw))
    # No interval shipped — return NaN bounds so classify_interval
    # reports MIDDLE (uncalibrated path) and the recipe defers to
    # the native point-estimate predicate verbatim. Returning a
    # zero-width interval would mis-classify as TIGHT and trigger a
    # spurious short-circuit on a row whose ``low`` is just the
    # point estimate.
    return (point, float("nan"), float("nan"))


def pick_target_vmaf_with_uncertainty(
    rows: Sequence[dict], req: UncertaintyAwareRequest
) -> UncertaintyRecommendResult:
    """Interval-aware analogue of :func:`pick_target_vmaf`.

    Search cost is :math:`O(k)` instead of :math:`O(n)` when at
    least one row's interval is tight enough that its lower bound
    clears ``target`` — the search short-circuits the moment that
    row is observed. Iteration order follows the input ``rows``
    (typically ascending CRF as produced by
    :func:`vmaftune.corpus.coarse_to_fine_search`); callers that
    care about a different traversal order pre-sort the input.

    Decision rules:

    * **Tight interval and ``low >= target``** — promote
      immediately; the conformal lower bound is a conservative
      lower-confidence proxy that already clears the bar
      (ADR-0279). Returned ``decision`` is
      :attr:`ConfidenceDecision.TIGHT`.
    * **Wide interval** — refuse to short-circuit on any single
      row; fall through to a full scan with the same point-
      estimate predicate :func:`pick_target_vmaf` uses.  Returned
      ``decision`` is :attr:`ConfidenceDecision.WIDE` and the
      ``predicate`` field includes ``(UNCERTAIN)``.
    * **Middle band** — defer to the native point-estimate
      predicate verbatim. Returned ``decision`` is
      :attr:`ConfidenceDecision.MIDDLE`.
    * **Interval excludes target across every visited row** — the
      whole corpus's interval is below ``target``; surface the
      best-effort row (highest VMAF) with ``predicate=...
      (UNMET)``.
    """
    eligible: list[dict] = []
    for r in rows:
        if req.encoder is not None and r.get("encoder") != req.encoder:
            continue
        if req.preset is not None and r.get("preset") != req.preset:
            continue
        if int(r.get("exit_status", 0)) != 0:
            continue
        v = r.get("vmaf_score")
        if v is None:
            continue
        try:
            fv = float(v)
        except (TypeError, ValueError):
            continue
        if not math.isfinite(fv):
            continue
        eligible.append(r)
    if not eligible:
        raise ValueError("no eligible rows to evaluate (after filtering)")

    target = req.target_vmaf
    thresholds = req.thresholds

    # Pass 1 — short-circuit search. Walk in input order; stop at
    # the first row whose interval is tight AND whose lower bound
    # already clears ``target``. Track the highest-VMAF row seen so
    # the UNMET branch can return a best-effort row without a
    # second pass.
    visited = 0
    best_so_far: dict | None = None
    best_score = -math.inf
    every_row_excludes = True
    saw_wide = False
    for r in eligible:
        visited += 1
        point, low, high = _row_interval(r, req)
        score = float(r["vmaf_score"])
        if score > best_score:
            best_score = score
            best_so_far = r
        if not interval_excludes_target(low=low, high=high, target=target):
            every_row_excludes = False
        # Preserve NaN through to ``classify_interval`` so an
        # uncalibrated row defers to MIDDLE band rather than being
        # mis-classified as zero-width TIGHT.
        if math.isnan(low) or math.isnan(high):
            width = float("nan")
        else:
            width = max(0.0, high - low)
        decision = classify_interval(width, thresholds)
        if decision is ConfidenceDecision.WIDE:
            saw_wide = True
        if decision is ConfidenceDecision.TIGHT and low >= target:
            return UncertaintyRecommendResult(
                row=r,
                predicate=f"target_vmaf>={target} (TIGHT, low={low:.3f})",
                margin=score - target,
                decision=ConfidenceDecision.TIGHT,
                visited=visited,
            )

    # No tight short-circuit fired. Decide the fallback strategy
    # from what we observed across the whole eligible set.
    if every_row_excludes:
        assert best_so_far is not None  # eligible is non-empty
        return UncertaintyRecommendResult(
            row=best_so_far,
            predicate=f"target_vmaf>={target} (UNMET, interval-excluded)",
            margin=best_score - target,
            decision=ConfidenceDecision.WIDE if saw_wide else ConfidenceDecision.MIDDLE,
            visited=visited,
        )

    # Fall back to the point-estimate predicate but tag the
    # decision band so callers can audit which recipe drove the
    # pick. saw_wide => WIDE; else MIDDLE.
    point_pick = pick_target_vmaf(eligible, target)
    band = ConfidenceDecision.WIDE if saw_wide else ConfidenceDecision.MIDDLE
    suffix = " (UNCERTAIN)" if band is ConfidenceDecision.WIDE else ""
    return UncertaintyRecommendResult(
        row=point_pick.row,
        predicate=f"{point_pick.predicate}{suffix}",
        margin=point_pick.margin,
        decision=band,
        visited=visited,
    )


__all__ = [
    "RecommendRequest",
    "RecommendResult",
    "UncertaintyAwareRequest",
    "UncertaintyRecommendResult",
    "format_result",
    "load_corpus_jsonl",
    "pick_target_bitrate",
    "pick_target_vmaf",
    "pick_target_vmaf_with_uncertainty",
    "recommend",
    "validate_request",
]
