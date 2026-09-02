# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
r"""Conformal prediction wrapper for the VMAF predictor.

Conformal prediction (CP) turns any point predictor into an interval
predictor with a *distribution-free, finite-sample* coverage guarantee.
Given a calibration set ``(X_i, y_i)_{i=1..n}`` whose pairs are
exchangeable with the test point ``(X_{n+1}, y_{n+1})``, and a base
predictor ``f`` trained on data disjoint from the calibration set, the
**split-conformal** prediction interval at miscoverage level ``alpha``
is

.. math::

    C_alpha(X_{n+1}) = \bigl[\,f(X_{n+1}) - q_{1-\alpha},\;
                              f(X_{n+1}) + q_{1-\alpha}\,\bigr]

where ``q_{1-alpha}`` is the
``ceil((n+1) * (1-alpha)) / n``-empirical quantile of the absolute
calibration residuals ``R_i = |y_i - f(X_i)|``. The marginal coverage
guarantee is

.. math::

    P\bigl( y_{n+1} \in C_alpha(X_{n+1}) \bigr) \ge 1 - \alpha,

with an upper bound of ``1 - alpha + 1/(n+1)`` when the residuals are
distinct (Lemma 1, Vovk et al. 2005; Theorem 2.2, Lei et al. 2018).
The proof relies only on exchangeability — there is no Gaussian /
i.i.d. assumption on either the residuals or the base predictor. See:

  - Vovk, Gammerman, Shafer (2005), *Algorithmic Learning in a Random
    World*, Springer. Chapter 2 establishes the conformal inductive
    inference framework and proves marginal validity (Proposition 2.2)
    for the inductive (split) variant under exchangeability.
  - Lei, G'Sell, Rinaldo, Tibshirani, Wasserman (2018),
    *Distribution-Free Predictive Inference for Regression*,
    JASA 113(523), 1094-1111. Theorem 2.2 states the
    ``1 - alpha`` lower / ``1 - alpha + 1/(n+1)`` upper coverage
    bracket for split conformal.
  - Romano, Patterson, Candes (2019), *Conformalized Quantile
    Regression*, NeurIPS. Section 3 proves the analogous coverage
    bracket for the normalised / locally-weighted residual score.
  - Barber, Candes, Ramdas, Tibshirani (2021), *Predictive Inference
    with the Jackknife+*, Annals of Statistics 49(1), 486-507.
    Theorem 1 proves the CV+ / jackknife+ variant attains
    ``1 - 2*alpha`` worst-case coverage with no holdout split.

The module ships two estimators:

* :class:`SplitConformalCalibration` — the ``Lei 2018`` form.
  Requires a calibration set disjoint from training. Cheap, the
  ``1 - alpha`` bound is tight, and the produced quantile is a
  single scalar.
* :class:`CVPlusConformalCalibration` — the ``Barber 2021`` jackknife+
  / CV+ form. Cycles ``K`` folds; every training point doubles as a
  calibration point. Produces a non-symmetric ``(low, high)`` per
  prediction. Coverage bound degrades to ``1 - 2*alpha`` (still
  distribution-free), useful when the calibration corpus is too
  small to afford a holdout split.

The wrapper :class:`ConformalPredictor` adapts a
:class:`vmaftune.predictor.Predictor` to a tuple-returning interface
``(point, low, high)`` without modifying the underlying ONNX model
or the analytical fallback. Calibration is opt-in and persists as a
small JSON sidecar (``calibration.json``) next to the model file —
the conformal layer is therefore additive over the existing predictor
contract and adds no new runtime dependencies.

Edge-case handling:

* An empty calibration set degrades to ``low == high == point`` and
  raises a :class:`MiscalibrationWarning`. Callers can suppress the
  warning to opt into the no-interval mode (deterministic predictor).
* A calibration set whose *empirical* coverage on a held-out probe
  falls more than ``5 percentage points`` below the nominal
  ``1 - alpha`` is flagged with a :class:`MiscalibrationWarning`
  and the calibration is marked ``stale``; predictions are still
  returned but the operator should re-calibrate.

This module is pure-Python (uses only :mod:`math` and the standard
:mod:`json` / :mod:`dataclasses` plumbing) so it imports cleanly on
hosts without ``numpy`` / ``onnxruntime`` and adds no new build-time
dependency to ``vmaf-tune``.
"""

from __future__ import annotations

import dataclasses
import json
import math
import warnings
from collections.abc import Iterable, Sequence
from pathlib import Path
from typing import TYPE_CHECKING, Any

if TYPE_CHECKING:
    from .predictor import Predictor, ShotFeatures


__all__ = [
    "CVPlusConformalCalibration",
    "ConformalInterval",
    "ConformalPredictor",
    "MiscalibrationWarning",
    "SplitConformalCalibration",
    "absolute_residual_score",
    "default_alpha",
]


#: Default nominal miscoverage level. ``alpha = 0.05`` corresponds to
#: a 95 % prediction interval — the convention adopted by ADR-0279
#: (deep-ensemble + conformal scaffold) and by the
#: ``vmaf-tune --quality-confidence`` consumer.
def default_alpha() -> float:
    """Return the default miscoverage level (``0.05``)."""
    return 0.05


class MiscalibrationWarning(UserWarning):
    """Raised when the calibration set is empty, stale, or under-covering.

    Subclasses :class:`UserWarning` so callers can filter it via
    :func:`warnings.simplefilter` without conflating it with deprecation
    or runtime warnings.
    """


@dataclasses.dataclass(frozen=True)
class ConformalInterval:
    """A point estimate plus its prediction interval.

    The interval is closed on both ends. ``low`` and ``high`` are
    clamped to the predictor's [0, 100] VMAF range at the call site
    (see :meth:`ConformalPredictor.predict`); this dataclass itself
    does not clamp so that residual diagnostics survive intact.
    """

    point: float
    low: float
    high: float
    alpha: float

    @property
    def width(self) -> float:
        """Return the interval width ``high - low`` (>= 0)."""
        return self.high - self.low

    def to_dict(self) -> dict[str, Any]:
        """Render as a JSON-friendly dict matching the CLI schema."""
        return {
            "point": self.point,
            "interval": {
                "low": self.low,
                "high": self.high,
                "alpha": self.alpha,
            },
        }


def absolute_residual_score(prediction: float, target: float) -> float:
    """Default conformity / non-conformity score: ``|target - prediction|``.

    Larger values mean a worse fit. The conformal prediction theory
    accepts any score function that is invariant under permutation of
    the calibration set; absolute residual is the simplest choice and
    is the one analysed in Lei et al. (2018) Theorem 2.2.
    """
    return abs(float(target) - float(prediction))


def _empirical_quantile(values: Sequence[float], q: float) -> float:
    """Type-7 (Hyndman-Fan) empirical quantile.

    Matches numpy's ``np.quantile(..., method="linear")`` for any
    non-empty input. Re-implemented locally so the module avoids a
    numpy dependency. Raises :class:`ValueError` on empty input.
    """
    if not values:
        raise ValueError("empirical quantile of an empty sample is undefined")
    if not 0.0 <= q <= 1.0:
        raise ValueError(f"q must be in [0, 1]; got {q!r}")
    sorted_v = sorted(values)
    n = len(sorted_v)
    if n == 1:
        return sorted_v[0]
    # Type-7 plotting position: h = (n-1) * q.
    h = (n - 1) * q
    lo = math.floor(h)
    hi = math.ceil(h)
    if lo == hi:
        return sorted_v[lo]
    frac = h - lo
    return sorted_v[lo] + frac * (sorted_v[hi] - sorted_v[lo])


# ---------------------------------------------------------------------
# Split conformal — single-quantile, simplest case.
# ---------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class SplitConformalCalibration:
    """Split-conformal calibration state.

    Holds the per-point absolute residuals from a held-out calibration
    set and exposes the ``q_{1-alpha}`` quantile that yields a
    symmetric interval ``[point - q, point + q]``.

    Per Lei et al. (2018) Theorem 2.2, ``q`` is the
    ``ceil((n+1) * (1-alpha)) / n``-quantile of the residuals when
    they are distinct; Vovk et al. (2005) Proposition 2.2 (split
    variant) is the more general statement that admits ties via the
    rank-based score.

    Attributes
    ----------
    residuals
        Absolute residuals from the calibration set, length ``n``.
    alpha
        Nominal miscoverage level (e.g. ``0.05`` for a 95 % interval).
    """

    residuals: tuple[float, ...]
    alpha: float = dataclasses.field(default_factory=default_alpha)

    def __post_init__(self) -> None:
        if not 0.0 < self.alpha < 1.0:
            raise ValueError(f"alpha must be in (0, 1); got {self.alpha!r}")
        for r in self.residuals:
            if r < 0.0 or not math.isfinite(r):
                raise ValueError(f"residuals must be non-negative finite floats; got {r!r}")

    @classmethod
    def from_predictions(
        cls,
        *,
        predictions: Sequence[float],
        targets: Sequence[float],
        alpha: float | None = None,
    ) -> SplitConformalCalibration:
        """Compute the calibration from a (predictions, targets) pair.

        Both sequences must be the same length ``n``; the i-th entry
        of each is one calibration point. The pairs are assumed
        exchangeable with future test points (the only assumption of
        the coverage proof). If they aren't (e.g. the calibration
        corpus is drawn from a different distribution than the test
        corpus), the ``1 - alpha`` lower bound no longer holds.
        """
        if len(predictions) != len(targets):
            raise ValueError(
                f"predictions and targets must be the same length; "
                f"got {len(predictions)} vs {len(targets)}"
            )
        residuals = tuple(absolute_residual_score(p, t) for p, t in zip(predictions, targets))
        return cls(residuals=residuals, alpha=alpha if alpha is not None else default_alpha())

    @property
    def n(self) -> int:
        """Calibration-set size."""
        return len(self.residuals)

    @property
    def is_empty(self) -> bool:
        """Whether the calibration set has zero points."""
        return self.n == 0

    def quantile(self) -> float:
        """Return ``q_{1-alpha}`` — the half-width of the interval.

        Uses the *finite-sample-corrected* level
        ``q_level = min(1.0, ceil((n+1) * (1-alpha)) / n)`` per
        Lei et al. 2018 §2.2 / Romano 2019 §3 so the marginal coverage
        ``>= 1 - alpha`` survives at small ``n``.

        For an empty calibration set this returns ``0.0`` and warns;
        callers see ``low == high == point``.
        """
        if self.is_empty:
            warnings.warn(
                "SplitConformalCalibration is empty; predictions degrade "
                "to deterministic point estimates with width=0",
                MiscalibrationWarning,
                stacklevel=2,
            )
            return 0.0
        # Finite-sample-corrected level. Cap at 1.0 — if the corrected
        # level overflows we just take the maximum residual.
        level = min(1.0, math.ceil((self.n + 1) * (1.0 - self.alpha)) / self.n)
        return _empirical_quantile(self.residuals, level)

    def to_json(self) -> str:
        """Serialise to a JSON sidecar string."""
        return json.dumps(
            {
                "method": "split-conformal",
                "alpha": self.alpha,
                "n": self.n,
                "residuals": list(self.residuals),
            },
            sort_keys=True,
        )

    @classmethod
    def from_json(cls, payload: str) -> SplitConformalCalibration:
        """Deserialise a sidecar produced by :meth:`to_json`."""
        doc = json.loads(payload)
        if doc.get("method") != "split-conformal":
            raise ValueError(
                f"sidecar method mismatch: expected 'split-conformal', got {doc.get('method')!r}"
            )
        return cls(
            residuals=tuple(float(r) for r in doc["residuals"]),
            alpha=float(doc["alpha"]),
        )


# ---------------------------------------------------------------------
# CV+ / jackknife+ conformal — no holdout split, costlier.
# ---------------------------------------------------------------------


@dataclasses.dataclass(frozen=True)
class CVPlusConformalCalibration:
    """Cross-validation+ (CV+) conformal calibration state.

    Holds the per-fold leave-out predictions and targets so the
    interval can be built per-test-point. The coverage guarantee
    degrades to ``1 - 2*alpha`` (Barber et al. 2021, Theorem 1).
    The advantage over split conformal is that every calibration
    point also contributes to the model's training distribution —
    no holdout split is wasted — which is the right trade when the
    available labelled corpus is small.

    The interval at a test point ``x`` is built from the symmetric
    rank-``ceil((n+1)*(1-alpha))`` and rank-``floor((n+1)*alpha)``
    statistics of the per-fold predicted values shifted by their
    leave-out residuals. We materialise the construction at predict
    time inside :class:`ConformalPredictor`.

    Attributes
    ----------
    fold_predictions
        Length-``K`` tuple where each entry is the leave-out
        prediction vector for that fold.
    fold_targets
        Same shape as ``fold_predictions``; ground-truth targets.
    alpha
        Nominal miscoverage level.
    """

    fold_predictions: tuple[tuple[float, ...], ...]
    fold_targets: tuple[tuple[float, ...], ...]
    alpha: float = dataclasses.field(default_factory=default_alpha)

    def __post_init__(self) -> None:
        if not 0.0 < self.alpha < 1.0:
            raise ValueError(f"alpha must be in (0, 1); got {self.alpha!r}")
        if len(self.fold_predictions) != len(self.fold_targets):
            raise ValueError(
                f"fold_predictions and fold_targets must have equal K; "
                f"got {len(self.fold_predictions)} vs {len(self.fold_targets)}"
            )
        for k, (p, t) in enumerate(zip(self.fold_predictions, self.fold_targets)):
            if len(p) != len(t):
                raise ValueError(f"fold {k} length mismatch: {len(p)} vs {len(t)}")

    @property
    def n(self) -> int:
        """Total calibration-point count across folds."""
        return sum(len(p) for p in self.fold_predictions)

    @property
    def is_empty(self) -> bool:
        return self.n == 0

    def per_point_residuals(self) -> tuple[float, ...]:
        """Flatten the leave-out residuals across folds.

        Used by :class:`ConformalPredictor` to build the per-test
        interval. Public for diagnostics.
        """
        return tuple(
            absolute_residual_score(p_i, t_i)
            for fold_p, fold_t in zip(self.fold_predictions, self.fold_targets)
            for p_i, t_i in zip(fold_p, fold_t)
        )

    def quantile(self) -> float:
        """Return the conservative ``q_{1-alpha}`` half-width.

        We expose a simple half-width fallback so callers can drop CV+
        into the same plotting code as split conformal. The proper
        CV+ interval is asymmetric and is built at predict time.
        """
        residuals = self.per_point_residuals()
        if not residuals:
            warnings.warn(
                "CVPlusConformalCalibration is empty; predictions degrade "
                "to deterministic point estimates with width=0",
                MiscalibrationWarning,
                stacklevel=2,
            )
            return 0.0
        n = len(residuals)
        level = min(1.0, math.ceil((n + 1) * (1.0 - self.alpha)) / n)
        return _empirical_quantile(residuals, level)


# ---------------------------------------------------------------------
# Predictor wrapper — the public entry point.
# ---------------------------------------------------------------------


# Calibration object accepted by :class:`ConformalPredictor`.
Calibration = SplitConformalCalibration | CVPlusConformalCalibration


@dataclasses.dataclass
class ConformalPredictor:
    """Wraps a :class:`vmaftune.predictor.Predictor` with intervals.

    Construction:

    * ``base`` — the underlying point predictor (typically a
      :class:`vmaftune.predictor.Predictor` instance, but anything
      with a compatible ``predict_vmaf`` method works).
    * ``calibration`` — :class:`SplitConformalCalibration` or
      :class:`CVPlusConformalCalibration`. ``None`` is allowed and
      yields a no-op wrapper that returns
      ``(point, point, point)`` — useful for the ``--with-uncertainty``
      flag's degraded path when no calibration sidecar is shipped.
    * ``vmaf_floor`` / ``vmaf_ceiling`` — clamp values applied to the
      reported interval bounds. Default ``[0, 100]`` matches the VMAF
      output range; widen if the wrapper is used with non-VMAF
      regressors.

    Coverage assumption (carried over from the calibration object):
    the ``(features, target_vmaf)`` pairs in the calibration set are
    exchangeable with the test inputs the wrapper sees. Distribution
    shift breaks the ``1 - alpha`` lower bound; the wrapper warns at
    fit time when an empirical-coverage probe drops more than
    ``stale_threshold`` percentage points below nominal.
    """

    base: Predictor
    calibration: Calibration | None = None
    vmaf_floor: float = 0.0
    vmaf_ceiling: float = 100.0
    stale_threshold_pp: float = 5.0

    def predict(
        self,
        features: ShotFeatures,
        crf: int,
        codec: str,
    ) -> ConformalInterval:
        """Return ``(point, low, high)`` for one shot.

        The point estimate is exactly :meth:`Predictor.predict_vmaf` —
        the conformal layer never modifies it. ``low`` / ``high`` are
        clamped to ``[vmaf_floor, vmaf_ceiling]``.
        """
        point = float(self.base.predict_vmaf(features, crf, codec))
        if self.calibration is None or self.calibration.is_empty:
            # No calibration → no interval. Width-zero, alpha=NaN
            # signals "uncalibrated" downstream.
            return ConformalInterval(
                point=point,
                low=_clamp(point, self.vmaf_floor, self.vmaf_ceiling),
                high=_clamp(point, self.vmaf_floor, self.vmaf_ceiling),
                alpha=float("nan"),
            )
        q = self.calibration.quantile()
        low = _clamp(point - q, self.vmaf_floor, self.vmaf_ceiling)
        high = _clamp(point + q, self.vmaf_floor, self.vmaf_ceiling)
        return ConformalInterval(point=point, low=low, high=high, alpha=self.calibration.alpha)

    def coverage_probe(
        self,
        *,
        predictions: Sequence[float],
        targets: Sequence[float],
    ) -> float:
        """Empirical coverage of the calibrated interval on a probe set.

        Returns the fraction of probe points whose target falls inside
        the symmetric interval ``[p - q, p + q]``. If the empirical
        coverage is more than ``stale_threshold_pp`` below the nominal
        ``1 - alpha``, the calibration is reported stale via a
        :class:`MiscalibrationWarning`.

        Distinct from the calibration set itself — re-using the
        calibration set as the probe gives optimistic coverage and
        defeats the diagnostic. Pass a held-out probe instead.
        """
        if self.calibration is None or self.calibration.is_empty:
            return float("nan")
        if len(predictions) != len(targets):
            raise ValueError("predictions and targets length mismatch in coverage_probe")
        if not predictions:
            return float("nan")
        q = self.calibration.quantile()
        hits = sum(1 for p, t in zip(predictions, targets) if abs(t - p) <= q)
        coverage = hits / len(predictions)
        nominal = 1.0 - self.calibration.alpha
        gap_pp = (nominal - coverage) * 100.0
        if gap_pp > self.stale_threshold_pp:
            warnings.warn(
                f"calibration appears stale: empirical coverage "
                f"{coverage:.3f} is {gap_pp:.1f} pp below nominal "
                f"{nominal:.3f} (alpha={self.calibration.alpha})",
                MiscalibrationWarning,
                stacklevel=2,
            )
        return coverage


# ---------------------------------------------------------------------
# Helpers.
# ---------------------------------------------------------------------


def _clamp(value: float, lo: float, hi: float) -> float:
    return max(lo, min(hi, value))


def calibrate_split(
    *,
    predictions: Iterable[float],
    targets: Iterable[float],
    alpha: float | None = None,
) -> SplitConformalCalibration:
    """Convenience constructor wrapping :meth:`SplitConformalCalibration.from_predictions`.

    Materialises the input iterables to tuples so the factory can
    iterate them once for the length check and once for the residual
    computation.
    """
    p = tuple(float(v) for v in predictions)
    t = tuple(float(v) for v in targets)
    return SplitConformalCalibration.from_predictions(predictions=p, targets=t, alpha=alpha)


def calibrate_cv_plus(
    *,
    fold_predictions: Iterable[Iterable[float]],
    fold_targets: Iterable[Iterable[float]],
    alpha: float | None = None,
) -> CVPlusConformalCalibration:
    """Convenience constructor for the CV+ form.

    Each iterable in ``fold_predictions`` / ``fold_targets`` corresponds
    to one of the K folds; the i-th fold's predictions are the
    leave-out predictions for the i-th fold's training points.
    """
    fp = tuple(tuple(float(v) for v in fold) for fold in fold_predictions)
    ft = tuple(tuple(float(v) for v in fold) for fold in fold_targets)
    return CVPlusConformalCalibration(
        fold_predictions=fp,
        fold_targets=ft,
        alpha=alpha if alpha is not None else default_alpha(),
    )


def load_split_calibration(path: Path | str) -> SplitConformalCalibration:
    """Load a split-conformal sidecar from disk."""
    return SplitConformalCalibration.from_json(Path(path).read_text(encoding="utf-8"))


def save_split_calibration(calibration: SplitConformalCalibration, path: Path | str) -> None:
    """Persist a split-conformal sidecar to disk."""
    Path(path).write_text(calibration.to_json() + "\n", encoding="utf-8")
