# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Shared uncertainty-aware recipe helpers for vmaf-tune.

The conformal-VQA prediction surface shipped in PR #488 (ADR-0279)
turns the predictor's verdict from binary GOSPEL / FALL_BACK into a
continuous (point, low, high) interval. PR #495 (Phase F.3 of the
``auto`` driver) carved an empirical-floor pair of width thresholds
out of Research-0067:

* ``tight_interval_max_width`` — width <= this -> the predictor is
  confident; downstream recipes can trust the point estimate and
  short-circuit search / ladder construction.
* ``wide_interval_min_width`` — width >= this -> the predictor is
  uncertain; downstream recipes should widen their search range or
  insert extra ladder rungs.
* tight < width < wide — middle band; defer to the native non-
  uncertainty recipe (preserves the existing recipe semantics
  exactly when the predictor is neither confident nor uncertain).

This module centralises the threshold dataclass + sidecar loader so
the three downstream consumers — :mod:`vmaftune.auto` (PR #495),
:mod:`vmaftune.recommend` and :mod:`vmaftune.ladder` (this PR) —
share the same defaults, the same sidecar schema, and the same
"emergency floor when no calibration ships" warning behaviour.

Threshold provenance (cited per the ``feedback_no_guessing`` rule in
``CLAUDE.md``):

* The defaults ``2.0`` / ``5.0`` VMAF come from Research-0067
  ("Phase F decision tree"), the same emergency floor PR #495's
  :func:`vmaftune.auto.load_confidence_thresholds` consumes. They
  are deliberately conservative: 2 VMAF is roughly a JND on a
  smoothed corpus, so an interval narrower than that is empirically
  indistinguishable from the point estimate; 5 VMAF spans more than
  one ABR rung's worth of perceptual quality, so an interval wider
  than that means the predictor cannot localise even the rung.
* Production sidecars override both via the JSON schema documented
  in :func:`load_confidence_thresholds`. The schema matches PR
  #495's loader byte-for-byte so a single sidecar can drive
  ``auto``, ``recommend`` and ``ladder`` without divergence.

Per the ``feedback_no_test_weakening`` rule in ``CLAUDE.md``: this
module *only* affects search-cost / ladder-rung selection. It does
**not** widen the production-flip gate (that gate lives in
``predictor_validate.py`` and stays untouched). Uncertainty recipes
change *which encodes get probed*, not *which encodes get shipped*.
"""

from __future__ import annotations

import dataclasses
import enum
import json
import logging
import math
from pathlib import Path

_LOG = logging.getLogger(__name__)


# Documented defaults from Research-0067 (Phase F feasibility study).
# Mirrored verbatim in :mod:`vmaftune.auto` (PR #495) so the two
# loaders agree when no calibration sidecar is shipped.
DEFAULT_TIGHT_INTERVAL_MAX_WIDTH: float = 2.0
DEFAULT_WIDE_INTERVAL_MIN_WIDTH: float = 5.0


class ConfidenceDecision(enum.Enum):
    """Per-call recipe override decision.

    Returned by :func:`classify_interval`. Downstream recipes consume
    the enum directly rather than re-deriving the band from the raw
    width — that keeps the gate semantics identical across consumers.

    * :attr:`TIGHT` — predictor is confident enough to short-circuit
      search / hold the ladder rung as authoritative.
    * :attr:`MIDDLE` — defer to the native (point-estimate) recipe;
      preserves the pre-uncertainty behaviour exactly.
    * :attr:`WIDE` — predictor is uncertain; widen search range /
      insert extra ladder rungs.
    """

    TIGHT = "tight"
    MIDDLE = "middle"
    WIDE = "wide"


@dataclasses.dataclass(frozen=True)
class ConfidenceThresholds:
    """Width thresholds carved from the calibration corpus.

    The two fields gate the per-call ``ConfidenceDecision``. Defaults
    are the emergency floor (Research-0067); production values come
    from a calibration sidecar produced by the conformal-VQA pipeline
    (ADR-0279 / PR #488). ``source`` records where the values came
    from for the JSON metadata block emitted by downstream recipes.

    A valid threshold pair satisfies
    ``0 < tight_interval_max_width <= wide_interval_min_width``. The
    constructor enforces this so a malformed sidecar fails fast
    rather than silently producing nonsense decisions.
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


def load_confidence_thresholds(sidecar_path: Path | str | None) -> ConfidenceThresholds:
    """Load corpus-derived thresholds from a calibration sidecar.

    The sidecar schema matches PR #495's ``auto`` loader byte-for-byte
    so a single sidecar drives every uncertainty-aware recipe::

        {
          "tight_interval_max_width": 1.6,
          "wide_interval_min_width": 4.2,
          ...
        }

    Extra keys are ignored so the loader survives schema growth.

    Failure modes (each returns the documented defaults and emits
    a one-line WARNING — never raises, so a missing sidecar
    degrades the recipe rather than killing the run):

    * ``None`` argument — caller did not pass a sidecar path.
    * Path does not exist on disk.
    * File is unreadable / not valid JSON / missing keys / wrong
      types / fails the ``tight <= wide`` invariant.

    The defaults are the *floor* surface — they keep the gate
    functional but signal that the corpus fit hasn't landed yet.
    """
    if sidecar_path is None:
        _LOG.warning(
            "vmaf-tune uncertainty: no calibration sidecar provided; "
            "falling back to documented defaults "
            "(tight=%.1f, wide=%.1f).",
            DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
            DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
        )
        return ConfidenceThresholds()
    path = Path(sidecar_path)
    if not path.exists():
        _LOG.warning(
            "vmaf-tune uncertainty: calibration sidecar %s not found; "
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
        thresholds = ConfidenceThresholds(
            tight_interval_max_width=tight,
            wide_interval_min_width=wide,
            source=str(path),
        )
    except (OSError, ValueError, KeyError, TypeError) as exc:
        _LOG.warning(
            "vmaf-tune uncertainty: calibration sidecar %s unreadable "
            "(%s); falling back to documented defaults "
            "(tight=%.1f, wide=%.1f).",
            path,
            exc,
            DEFAULT_TIGHT_INTERVAL_MAX_WIDTH,
            DEFAULT_WIDE_INTERVAL_MIN_WIDTH,
        )
        return ConfidenceThresholds()
    return thresholds


def classify_interval(
    interval_width: float, thresholds: ConfidenceThresholds
) -> ConfidenceDecision:
    """Carve a single interval width into the three confidence bands.

    Pure function over ``(width, thresholds)`` so unit tests can
    sweep the band edges without standing up a predictor.

    * ``NaN`` width (uncalibrated predictor) -> :attr:`MIDDLE`. The
      caller falls back to the native non-uncertainty recipe; the
      F.3 / recommend / ladder behaviour collapses to its
      pre-uncertainty form, which is exactly what the
      "no calibration shipped" path needs.
    * Negative width -> :class:`ValueError`. Conformal interval
      widths are always non-negative by construction; a negative
      value indicates a caller bug, not a calibration issue.
    """
    width = float(interval_width)
    if math.isnan(width):
        return ConfidenceDecision.MIDDLE
    if width < 0.0:
        raise ValueError(f"classify_interval: interval_width must be >= 0.0 or NaN; got {width!r}")
    if width <= thresholds.tight_interval_max_width:
        return ConfidenceDecision.TIGHT
    if width >= thresholds.wide_interval_min_width:
        return ConfidenceDecision.WIDE
    return ConfidenceDecision.MIDDLE


def interval_excludes_target(*, low: float, high: float, target: float, slack: float = 0.0) -> bool:
    """Whether the prediction interval lies strictly above or below ``target``.

    Used by ``recommend`` to short-circuit the CRF search when the
    predictor's interval has already determined that *no* CRF in the
    current bracket can hit ``target``. ``slack`` widens the
    "definitely-misses" zone — pass ``slack > 0`` to require the
    interval to miss by more than ``slack`` VMAF before short-
    circuiting.

    Returns ``True`` when ``high < target - slack`` (the whole
    interval is below the target with margin) or ``low > target +
    slack`` (the whole interval is above the target with margin).
    """
    if not math.isfinite(low) or not math.isfinite(high):
        return False
    if high < target - slack:
        return True
    return low > target + slack


__all__ = [
    "DEFAULT_TIGHT_INTERVAL_MAX_WIDTH",
    "DEFAULT_WIDE_INTERVAL_MIN_WIDTH",
    "ConfidenceDecision",
    "ConfidenceThresholds",
    "classify_interval",
    "interval_excludes_target",
    "load_confidence_thresholds",
]
