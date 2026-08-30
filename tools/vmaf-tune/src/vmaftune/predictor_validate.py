# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Validation harness — verify the VMAF predictor on a few real shots.

The colleague's ask: "verify a few of those shots against real vmaf".
This module is the verify half of the predict-then-verify loop:

1.  :func:`select_validation_shots` — picks K shots from the full set,
    stratified by the predictor's complexity proxy (probe bitrate
    quartile) so the validation samples the predictor's full operating
    range, not just the easy shots.
2.  :func:`validate_predictor` — for each selected shot, runs the real
    encode at the predictor-picked CRF, scores with libvmaf via
    :mod:`vmaftune.score`, and computes residuals.
3.  Emits a :class:`Verdict`:

    * ``GOSPEL`` — every residual within
      ``residual_threshold_vmaf`` (default 1.5). Trust the predictor on
      the remaining shots.
    * ``RECALIBRATE`` — residuals biased but tight. Apply a per-movie
      one-parameter linear shift correction, redo the picks. Cheap;
      doesn't require retraining the ONNX model.
    * ``FALL_BACK`` — residuals too wide. Degrade to the full
      encode-and-score loop on remaining shots and log to follow-up.

The 3-4 % overhead budget the colleague cares about: K=8 on a
1 800-shot movie is 0.4 % for validation. The probe encodes themselves
(in :mod:`vmaftune.predictor_features`) add ~2-3 % on top. Totals fit
the budget.
"""

from __future__ import annotations

import dataclasses
import enum
from collections.abc import Callable, Sequence
from pathlib import Path
from typing import TYPE_CHECKING

from .per_shot import Shot
from .predictor import Predictor, ShotFeatures

if TYPE_CHECKING:
    pass

# Callback that turns a (shot, target_vmaf, codec) into a real
# (encoded_path, measured_vmaf). Real implementation runs the encoder
# + libvmaf; tests inject a deterministic stub.
RealEncodeAndScore = Callable[[Shot, int, str], tuple[Path, float]]

# Callback the harness uses to extract per-shot features. The real
# version is :func:`vmaftune.predictor_features.extract_features` —
# we accept it as a parameter so the validate path doesn't need an
# import of the heavy feature module in the test path.
FeatureExtractor = Callable[[Shot], ShotFeatures]


class Verdict(enum.Enum):
    """Outcome of a predictor validation run."""

    GOSPEL = "gospel"
    RECALIBRATE = "recalibrate"
    FALL_BACK = "fall_back"


@dataclasses.dataclass(frozen=True)
class ShotResidual:
    """One validation point: predictor said X, real VMAF was Y."""

    shot: Shot
    crf_picked: int
    predicted_vmaf: float
    measured_vmaf: float

    @property
    def residual(self) -> float:
        """Signed residual: ``measured − predicted``.

        Sign matters — biased-low residuals (predictor over-estimates
        quality) are worse than biased-high (over-estimates encode
        cost) for downstream gating.
        """
        return self.measured_vmaf - self.predicted_vmaf


@dataclasses.dataclass(frozen=True)
class ValidationReport:
    """Summary of a validation run."""

    verdict: Verdict
    residuals: tuple[ShotResidual, ...]
    target_vmaf: float
    threshold_vmaf: float
    bias_correction: float = 0.0  # signed, only set when verdict == RECALIBRATE

    @property
    def max_abs_residual(self) -> float:
        if not self.residuals:
            return 0.0
        return max(abs(r.residual) for r in self.residuals)

    @property
    def mean_residual(self) -> float:
        if not self.residuals:
            return 0.0
        return sum(r.residual for r in self.residuals) / len(self.residuals)


def select_validation_shots(
    shots: Sequence[Shot],
    features_by_shot: dict[Shot, ShotFeatures],
    *,
    k: int = 8,
    strategy: str = "stratified",
) -> tuple[Shot, ...]:
    """Pick ``k`` shots from ``shots`` to validate.

    The default ``stratified`` strategy ranks shots by probe bitrate
    and splits into 4 quartiles; ``k // 4`` shots are drawn from each.
    When ``k`` isn't a multiple of 4 the remainder lands in the highest
    quartile (where the predictor is most likely to be wrong, so extra
    samples there are well-spent).

    The ``head`` strategy picks the first ``k`` shots — useful only
    for tests where deterministic order matters more than coverage.
    """
    if k <= 0:
        return ()
    if not shots:
        return ()
    if len(shots) <= k:
        return tuple(shots)
    if strategy == "head":
        return tuple(shots[:k])
    if strategy != "stratified":
        raise ValueError(f"unknown strategy {strategy!r}; expected 'stratified' or 'head'")

    # Stratify by probe bitrate. Missing entries fall to bitrate 0 so
    # they cluster in the lowest quartile (as content the probe
    # couldn't measure they're tail-content the predictor will likely
    # mis-judge).
    ranked = sorted(
        shots,
        key=lambda s: features_by_shot.get(s, ShotFeatures(0.0, 0.0, 0.0, 0.0)).probe_bitrate_kbps,
    )
    quartile_size = len(ranked) // 4
    per_quartile = k // 4
    extra = k - per_quartile * 4
    selected: list[Shot] = []
    for i in range(4):
        # Quartile i covers ranked[i*qs:(i+1)*qs] (last quartile takes
        # the remainder). Pick evenly-spaced indices within the
        # quartile so we don't double-sample adjacent shots.
        lo = i * quartile_size
        hi = (i + 1) * quartile_size if i < 3 else len(ranked)
        quartile = ranked[lo:hi]
        if not quartile:
            continue
        n_pick = per_quartile + (extra if i == 3 else 0)
        n_pick = min(n_pick, len(quartile))
        if n_pick == 0:
            continue
        # Evenly spaced within the quartile.
        step = max(len(quartile) // n_pick, 1)
        for j in range(n_pick):
            idx = min(j * step, len(quartile) - 1)
            selected.append(quartile[idx])
    return tuple(selected)


def validate_predictor(
    predictor: Predictor,
    shots: Sequence[Shot],
    target_vmaf: float,
    codec: str,
    feature_extractor: FeatureExtractor,
    real_encode_and_score: RealEncodeAndScore,
    *,
    k: int = 8,
    residual_threshold_vmaf: float = 1.5,
    selection_strategy: str = "stratified",
) -> ValidationReport:
    """Validate ``predictor`` against real libvmaf scores on ``k`` shots.

    Workflow:

    1. Compute features for every shot (cheap; one probe encode each).
    2. Pick ``k`` shots via :func:`select_validation_shots`.
    3. For each selected shot, ask the predictor for ``(crf, vmaf)``,
       run the real encode + libvmaf score, and compute the residual.
    4. Decide :class:`Verdict` from the residuals.

    If verdict is ``RECALIBRATE``, the report includes a
    ``bias_correction`` — signed VMAF offset to add to predictions on
    the remaining (un-validated) shots.
    """
    features_by_shot: dict[Shot, ShotFeatures] = {s: feature_extractor(s) for s in shots}
    selected = select_validation_shots(shots, features_by_shot, k=k, strategy=selection_strategy)

    residuals: list[ShotResidual] = []
    for shot in selected:
        feats = features_by_shot[shot]
        crf = predictor.pick_crf(feats, target_vmaf, codec)
        predicted = predictor.predict_vmaf(feats, crf, codec)
        _, measured = real_encode_and_score(shot, crf, codec)
        residuals.append(
            ShotResidual(
                shot=shot,
                crf_picked=crf,
                predicted_vmaf=predicted,
                measured_vmaf=measured,
            )
        )

    return _decide_verdict(
        residuals=tuple(residuals),
        target_vmaf=target_vmaf,
        threshold=residual_threshold_vmaf,
    )


def _decide_verdict(
    residuals: tuple[ShotResidual, ...],
    target_vmaf: float,
    threshold: float,
) -> ValidationReport:
    """Map a residuals list to a :class:`ValidationReport`."""
    if not residuals:
        # Empty selection — pessimistic verdict so the caller
        # falls back to full encode-and-score on every shot.
        return ValidationReport(
            verdict=Verdict.FALL_BACK,
            residuals=residuals,
            target_vmaf=target_vmaf,
            threshold_vmaf=threshold,
        )

    abs_residuals = [abs(r.residual) for r in residuals]
    max_abs = max(abs_residuals)
    mean_signed = sum(r.residual for r in residuals) / len(residuals)
    # Spread = max - min of signed residuals; tight spread + biased
    # mean = recalibrate. Wide spread = fall back regardless of mean.
    signed = [r.residual for r in residuals]
    spread = max(signed) - min(signed)

    if max_abs <= threshold:
        return ValidationReport(
            verdict=Verdict.GOSPEL,
            residuals=residuals,
            target_vmaf=target_vmaf,
            threshold_vmaf=threshold,
        )

    # Tight spread (within threshold) but biased mean → recalibrate.
    # The bias correction is the signed mean residual, applied as an
    # additive shift to predictions on the remaining shots.
    if spread <= 2.0 * threshold and abs(mean_signed) > threshold:
        return ValidationReport(
            verdict=Verdict.RECALIBRATE,
            residuals=residuals,
            target_vmaf=target_vmaf,
            threshold_vmaf=threshold,
            bias_correction=mean_signed,
        )

    return ValidationReport(
        verdict=Verdict.FALL_BACK,
        residuals=residuals,
        target_vmaf=target_vmaf,
        threshold_vmaf=threshold,
    )


__all__ = [
    "FeatureExtractor",
    "RealEncodeAndScore",
    "ShotResidual",
    "ValidationReport",
    "Verdict",
    "select_validation_shots",
    "validate_predictor",
]
