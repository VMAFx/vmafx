# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Tests for the conformal prediction wrapper.

Pins:

* The empirical coverage on a synthetic Gaussian-noise corpus matches
  the nominal ``1 - alpha`` within a tolerance proportional to the
  test-set size — the marginal-coverage proof from
  Lei et al. 2018 Theorem 2.2 in action.
* The interval width is deterministic for a fixed calibration set
  and increases monotonically with ``alpha`` (tighter coverage =>
  wider interval).
* An empty calibration set degrades to ``low == high == point`` and
  emits a :class:`MiscalibrationWarning`.
* A miscalibrated probe (target distribution shifted off the
  calibration distribution) trips the stale-calibration warning.
* The serialisation round-trips a sidecar produced by
  :func:`save_split_calibration` byte-for-byte equal to the original.
* CV+ on a small fixture clears the
  ``1 - 2*alpha`` worst-case bound (Barber 2021 Theorem 1).
"""

from __future__ import annotations

import json
import random
import sys
import warnings
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
_SRC = _HERE.parent / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

from vmaftune.conformal import (  # noqa: E402
    ConformalInterval,
    ConformalPredictor,
    CVPlusConformalCalibration,
    MiscalibrationWarning,
    SplitConformalCalibration,
    absolute_residual_score,
    calibrate_cv_plus,
    calibrate_split,
    default_alpha,
    load_split_calibration,
    save_split_calibration,
)
from vmaftune.predictor import Predictor, ShotFeatures  # noqa: E402

# ---------------------------------------------------------------------
# Fixtures: synthetic calibration set with known noise model.
# ---------------------------------------------------------------------


def _synthetic_pairs(
    *,
    n: int,
    sigma: float,
    seed: int,
) -> tuple[list[float], list[float]]:
    """Return ``(predictions, targets)`` with ``targets ~ predictions + N(0, sigma^2)``.

    Predictions are uniform on ``[60, 95]`` so the synthetic distribution
    sits inside the VMAF range. Using :mod:`random.gauss` keeps the
    corpus reproducible without numpy.
    """
    rng = random.Random(seed)
    predictions = [rng.uniform(60.0, 95.0) for _ in range(n)]
    targets = [p + rng.gauss(0.0, sigma) for p in predictions]
    return predictions, targets


# ---------------------------------------------------------------------
# Tests — split conformal.
# ---------------------------------------------------------------------


def test_split_conformal_attains_nominal_coverage_on_synthetic_gaussian() -> None:
    """The 95% interval covers ~95% of held-out points (within MC noise)."""
    cal_p, cal_t = _synthetic_pairs(n=400, sigma=2.0, seed=0xC0FFEE)
    test_p, test_t = _synthetic_pairs(n=2000, sigma=2.0, seed=0xBEEF)

    calibration = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    q = calibration.quantile()
    hits = sum(1 for p, t in zip(test_p, test_t) if abs(t - p) <= q)
    coverage = hits / len(test_p)

    # Marginal-coverage bound: 1 - alpha <= coverage <= 1 - alpha + 1/(n+1).
    # With n_cal = 400 the upper bracket is ~0.952; tolerate 2.5 pp of MC
    # noise on a 2000-point probe (sqrt(0.05*0.95/2000) ~= 0.005, so
    # 4-sigma room).
    assert 0.93 <= coverage <= 0.985, coverage


def test_split_conformal_interval_widens_with_alpha() -> None:
    """Lower miscoverage (=> higher coverage) yields wider intervals."""
    cal_p, cal_t = _synthetic_pairs(n=200, sigma=3.0, seed=0xFADE)
    cal_05 = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    cal_10 = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.10)
    cal_20 = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.20)
    assert cal_05.quantile() >= cal_10.quantile() >= cal_20.quantile()


def test_split_conformal_quantile_is_deterministic() -> None:
    """The quantile depends only on the residual multiset, not order."""
    cal_p, cal_t = _synthetic_pairs(n=200, sigma=2.5, seed=42)
    cal_a = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    # Permute the inputs deterministically.
    perm_p = list(reversed(cal_p))
    perm_t = list(reversed(cal_t))
    cal_b = calibrate_split(predictions=perm_p, targets=perm_t, alpha=0.05)
    assert cal_a.quantile() == pytest.approx(cal_b.quantile())


def test_split_conformal_finite_sample_correction() -> None:
    """At small ``n``, the corrected level may equal 1.0 (cap the level)."""
    # n = 5, alpha = 0.05 => ceil(6 * 0.95) / 5 = ceil(5.7)/5 = 6/5 = 1.2
    # capped at 1.0 → take the maximum residual.
    cal = SplitConformalCalibration(residuals=(1.0, 2.0, 3.0, 4.0, 5.0), alpha=0.05)
    assert cal.quantile() == pytest.approx(5.0)


def test_split_conformal_round_trip_sidecar(tmp_path: Path) -> None:
    """Sidecar JSON serialisation is loss-less."""
    cal_p, cal_t = _synthetic_pairs(n=50, sigma=1.0, seed=7)
    original = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.10)
    sidecar = tmp_path / "calibration.json"
    save_split_calibration(original, sidecar)
    restored = load_split_calibration(sidecar)
    assert restored.alpha == original.alpha
    assert restored.residuals == original.residuals
    assert restored.quantile() == pytest.approx(original.quantile())


def test_split_conformal_rejects_invalid_alpha() -> None:
    with pytest.raises(ValueError, match="alpha must be in"):
        SplitConformalCalibration(residuals=(1.0,), alpha=0.0)
    with pytest.raises(ValueError, match="alpha must be in"):
        SplitConformalCalibration(residuals=(1.0,), alpha=1.0)


def test_split_conformal_rejects_negative_residuals() -> None:
    with pytest.raises(ValueError, match="non-negative"):
        SplitConformalCalibration(residuals=(-0.1,), alpha=0.05)


def test_split_conformal_rejects_length_mismatch() -> None:
    with pytest.raises(ValueError, match="same length"):
        SplitConformalCalibration.from_predictions(
            predictions=[1.0, 2.0], targets=[3.0], alpha=0.05
        )


def test_empty_calibration_warns_and_returns_zero_width() -> None:
    cal = SplitConformalCalibration(residuals=(), alpha=0.05)
    assert cal.is_empty
    with warnings.catch_warnings(record=True) as captured:
        warnings.simplefilter("always")
        q = cal.quantile()
    assert q == 0.0
    assert any(issubclass(w.category, MiscalibrationWarning) for w in captured)


def test_absolute_residual_score_is_symmetric_in_arguments() -> None:
    assert absolute_residual_score(7.0, 3.0) == absolute_residual_score(3.0, 7.0) == 4.0


# ---------------------------------------------------------------------
# Tests — ConformalPredictor wrapper integration.
# ---------------------------------------------------------------------


def _features() -> ShotFeatures:
    return ShotFeatures(
        probe_bitrate_kbps=2500.0,
        probe_i_frame_avg_bytes=12000.0,
        probe_p_frame_avg_bytes=4000.0,
        probe_b_frame_avg_bytes=2000.0,
        shot_length_frames=120,
        fps=24.0,
        width=1920,
        height=1080,
    )


def test_wrapper_with_no_calibration_returns_zero_width() -> None:
    pred = Predictor()
    wrapper = ConformalPredictor(base=pred)
    result = wrapper.predict(_features(), crf=23, codec="libx264")
    assert isinstance(result, ConformalInterval)
    assert result.low == result.high == result.point


def test_wrapper_with_calibration_returns_symmetric_interval() -> None:
    pred = Predictor()
    cal = SplitConformalCalibration(residuals=(0.5, 1.0, 1.5, 2.0, 2.5), alpha=0.05)
    wrapper = ConformalPredictor(base=pred, calibration=cal)
    result = wrapper.predict(_features(), crf=23, codec="libx264")
    q = cal.quantile()
    assert result.high - result.point == pytest.approx(min(q, 100.0 - result.point))
    assert result.point - result.low == pytest.approx(min(q, result.point))


def test_wrapper_clamps_to_vmaf_range() -> None:
    """Intervals are clamped into [0, 100] — VMAF can't go negative."""
    pred = Predictor()
    # Force a huge quantile so the lower bound would underflow.
    cal = SplitConformalCalibration(residuals=tuple([200.0] * 30), alpha=0.05)
    wrapper = ConformalPredictor(base=pred, calibration=cal)
    result = wrapper.predict(_features(), crf=23, codec="libx264")
    assert result.low == 0.0
    assert result.high == 100.0


def test_wrapper_alpha_override_via_predictor() -> None:
    """``Predictor.predict_vmaf_with_uncertainty`` honours an alpha override."""
    pred = Predictor()
    cal_p, cal_t = _synthetic_pairs(n=200, sigma=2.0, seed=21)
    cal = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    point, low_05, high_05 = pred.predict_vmaf_with_uncertainty(
        _features(), 23, "libx264", calibration=cal
    )
    _, low_20, high_20 = pred.predict_vmaf_with_uncertainty(
        _features(), 23, "libx264", calibration=cal, alpha=0.20
    )
    # Wider coverage (alpha=0.05) → wider interval than alpha=0.20.
    assert (high_05 - low_05) >= (high_20 - low_20)
    assert low_05 <= point <= high_05


def test_coverage_probe_flags_stale_calibration() -> None:
    """A probe drawn from a worse-noise distribution trips the stale warning."""
    pred = Predictor()
    cal_p, cal_t = _synthetic_pairs(n=300, sigma=1.0, seed=33)
    calibration = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    wrapper = ConformalPredictor(base=pred, calibration=calibration, stale_threshold_pp=2.0)

    # Probe with 5x the noise of the calibration set — empirical
    # coverage will collapse far below 95 %.
    probe_p, probe_t = _synthetic_pairs(n=500, sigma=5.0, seed=99)
    with warnings.catch_warnings(record=True) as captured:
        warnings.simplefilter("always")
        coverage = wrapper.coverage_probe(predictions=probe_p, targets=probe_t)
    assert coverage < 0.93  # dropped well below nominal 0.95
    assert any(issubclass(w.category, MiscalibrationWarning) for w in captured)


def test_coverage_probe_on_well_calibrated_data_does_not_warn() -> None:
    pred = Predictor()
    cal_p, cal_t = _synthetic_pairs(n=400, sigma=2.0, seed=11)
    calibration = calibrate_split(predictions=cal_p, targets=cal_t, alpha=0.05)
    wrapper = ConformalPredictor(base=pred, calibration=calibration)

    probe_p, probe_t = _synthetic_pairs(n=400, sigma=2.0, seed=12)
    with warnings.catch_warnings(record=True) as captured:
        warnings.simplefilter("always")
        coverage = wrapper.coverage_probe(predictions=probe_p, targets=probe_t)
    assert coverage > 0.92
    assert not any(issubclass(w.category, MiscalibrationWarning) for w in captured)


def test_interval_width_correlates_with_calibration_noise() -> None:
    """Intervals are wider when the calibration corpus is noisier.

    Surrogate for "interval width correlates with prediction
    difficulty" — a noisier corpus represents harder-to-predict
    content, and conformal therefore widens accordingly.
    """
    cal_easy_p, cal_easy_t = _synthetic_pairs(n=300, sigma=1.0, seed=51)
    cal_hard_p, cal_hard_t = _synthetic_pairs(n=300, sigma=4.0, seed=51)
    cal_easy = calibrate_split(predictions=cal_easy_p, targets=cal_easy_t, alpha=0.05)
    cal_hard = calibrate_split(predictions=cal_hard_p, targets=cal_hard_t, alpha=0.05)
    assert cal_hard.quantile() > cal_easy.quantile()


# ---------------------------------------------------------------------
# Tests — CV+ conformal.
# ---------------------------------------------------------------------


def test_cv_plus_clears_2alpha_bound_on_small_fixture() -> None:
    """Empirical coverage clears Barber 2021 Theorem 1's 1 - 2*alpha."""
    rng = random.Random(0xCA7CA7)
    # 5 folds of 40 leave-out points each.
    fold_predictions = []
    fold_targets = []
    for _ in range(5):
        preds = [rng.uniform(60.0, 95.0) for _ in range(40)]
        targets = [p + rng.gauss(0.0, 1.5) for p in preds]
        fold_predictions.append(preds)
        fold_targets.append(targets)

    cal = calibrate_cv_plus(
        fold_predictions=fold_predictions,
        fold_targets=fold_targets,
        alpha=0.10,
    )
    q = cal.quantile()

    # Held-out probe drawn from the same distribution.
    probe_preds = [rng.uniform(60.0, 95.0) for _ in range(2000)]
    probe_targets = [p + rng.gauss(0.0, 1.5) for p in probe_preds]
    hits = sum(1 for p, t in zip(probe_preds, probe_targets) if abs(t - p) <= q)
    coverage = hits / len(probe_preds)
    assert coverage >= 1 - 2 * cal.alpha, (coverage, cal.alpha)


def test_cv_plus_is_empty_short_circuits() -> None:
    cal = CVPlusConformalCalibration(fold_predictions=(), fold_targets=(), alpha=0.05)
    assert cal.is_empty
    with warnings.catch_warnings(record=True) as captured:
        warnings.simplefilter("always")
        q = cal.quantile()
    assert q == 0.0
    assert any(issubclass(w.category, MiscalibrationWarning) for w in captured)


def test_cv_plus_rejects_fold_length_mismatch() -> None:
    with pytest.raises(ValueError, match="length mismatch"):
        CVPlusConformalCalibration(
            fold_predictions=((1.0, 2.0),),
            fold_targets=((1.0,),),
            alpha=0.05,
        )


def test_cv_plus_rejects_fold_count_mismatch() -> None:
    with pytest.raises(ValueError, match="equal K"):
        CVPlusConformalCalibration(
            fold_predictions=((1.0,), (2.0,)),
            fold_targets=((1.0,),),
            alpha=0.05,
        )


# ---------------------------------------------------------------------
# Tests — sidecar parsing edge cases.
# ---------------------------------------------------------------------


def test_load_split_calibration_rejects_wrong_method(tmp_path: Path) -> None:
    sidecar = tmp_path / "wrong.json"
    sidecar.write_text(json.dumps({"method": "bogus", "alpha": 0.05, "residuals": []}))
    with pytest.raises(ValueError, match="method mismatch"):
        load_split_calibration(sidecar)


def test_default_alpha_is_five_percent() -> None:
    assert default_alpha() == 0.05


def test_to_dict_renders_cli_schema() -> None:
    interval = ConformalInterval(point=87.3, low=85.2, high=89.4, alpha=0.05)
    payload = interval.to_dict()
    assert payload == {
        "point": 87.3,
        "interval": {"low": 85.2, "high": 89.4, "alpha": 0.05},
    }
