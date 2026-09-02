# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Unit tests for parse_feature_aggregates with real libvmaf integer_* JSON keys.

Modern libvmaf emits pooled_metrics keys prefixed with ``integer_``
(e.g. ``integer_adm2``, ``integer_vif_scale0``).  The canonical-6 bare
names (``adm2``, ``vif_scale0``, …) must be resolved to those prefixed
keys; otherwise every corpus row silently carries NaN for all 12
per-feature columns.

Regression guard for the bug surfaced by matrix v2 (ADR-0366 follow-up).
"""

from __future__ import annotations

import math
import sys
from pathlib import Path

import pytest

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE.parent / "src"))

from vmaftune import CANONICAL6_FEATURES
from vmaftune.score import parse_feature_aggregates

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_integer_pooled_payload(*, with_stddev: bool = False) -> dict:
    """Synthesise a pooled_metrics payload mirroring real libvmaf integer output.

    Keys follow the ``integer_*`` convention that libvmaf's integer
    pipeline emits.  ``harmonic_mean`` is included (as in the real
    binary); ``stddev`` is optionally added so we can test that path too.
    """
    integer_values = {
        "integer_adm2": {
            "min": 0.980,
            "max": 1.000,
            "mean": 0.9878,
            "harmonic_mean": 0.9878,
        },
        "integer_vif_scale0": {
            "min": 0.805,
            "max": 1.000,
            "mean": 0.8263,
            "harmonic_mean": 0.8249,
        },
        "integer_vif_scale1": {
            "min": 0.971,
            "max": 1.000,
            "mean": 0.9768,
            "harmonic_mean": 0.9768,
        },
        "integer_vif_scale2": {
            "min": 0.984,
            "max": 1.000,
            "mean": 0.9882,
            "harmonic_mean": 0.9882,
        },
        "integer_vif_scale3": {
            "min": 0.990,
            "max": 1.000,
            "mean": 0.9933,
            "harmonic_mean": 0.9933,
        },
        "integer_motion2": {
            "min": 0.000,
            "max": 45.47,
            "mean": 3.2967,
            "harmonic_mean": 0.1625,
        },
    }
    if with_stddev:
        std_values = {
            "integer_adm2": 0.005,
            "integer_vif_scale0": 0.010,
            "integer_vif_scale1": 0.007,
            "integer_vif_scale2": 0.004,
            "integer_vif_scale3": 0.003,
            "integer_motion2": 1.200,
        }
        for key, val in std_values.items():
            integer_values[key]["stddev"] = val
    pooled = {"vmaf": {"min": 87.0, "max": 95.0, "mean": 91.3}}
    pooled.update(integer_values)
    return {"pooled_metrics": pooled}


# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------


class TestParseFeatureAggregatesIntegerKeys:
    """parse_feature_aggregates resolves integer_* pooled keys correctly."""

    def test_all_six_means_non_nan_with_integer_keys(self) -> None:
        """All 6 canonical features resolve to finite means from integer_* keys."""
        payload = _make_integer_pooled_payload()
        means, _stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert set(means.keys()) == set(
            CANONICAL6_FEATURES
        ), f"Expected all 6 features in means; got {set(means.keys())}"
        for name in CANONICAL6_FEATURES:
            assert math.isfinite(
                means[name]
            ), f"{name}_mean is NaN/Inf — integer_* key resolution broken"

    def test_mean_values_match_fixture(self) -> None:
        """Resolved mean values match the synthesised integer_* fixture."""
        payload = _make_integer_pooled_payload()
        means, _ = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert means["adm2"] == pytest.approx(0.9878)
        assert means["vif_scale0"] == pytest.approx(0.8263)
        assert means["vif_scale1"] == pytest.approx(0.9768)
        assert means["vif_scale2"] == pytest.approx(0.9882)
        assert means["vif_scale3"] == pytest.approx(0.9933)
        assert means["motion2"] == pytest.approx(3.2967)

    def test_stddev_parsed_when_present(self) -> None:
        """stddev values are extracted when present in the integer_* block."""
        payload = _make_integer_pooled_payload(with_stddev=True)
        _means, stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert len(stds) == 6
        assert stds["motion2"] == pytest.approx(1.200)

    def test_no_stddev_returns_empty_stds(self) -> None:
        """stds dict is empty when integer_* blocks lack stddev (normal case)."""
        payload = _make_integer_pooled_payload(with_stddev=False)
        _, stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert stds == {}

    def test_bare_key_fallback_for_non_integer_features(self) -> None:
        """Non-integer features (cambi) resolve via bare-key fallback."""
        payload = {
            "pooled_metrics": {
                "vmaf": {"mean": 88.0},
                "cambi": {"mean": 0.42, "stddev": 0.05},
            }
        }
        means, stds = parse_feature_aggregates(payload, ("cambi",))
        assert means.get("cambi") == pytest.approx(0.42)
        assert stds.get("cambi") == pytest.approx(0.05)

    def test_absent_features_not_in_means(self) -> None:
        """Features absent from pooled_metrics do not appear in the output dicts."""
        # Only vif_scale0 present (as integer key).
        payload = {
            "pooled_metrics": {
                "vmaf": {"mean": 91.0},
                "integer_vif_scale0": {"min": 0.8, "max": 1.0, "mean": 0.83},
            }
        }
        means, stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert set(means.keys()) == {
            "vif_scale0"
        }, f"Only vif_scale0 expected; got {set(means.keys())}"
        assert stds == {}

    def test_empty_pooled_metrics_returns_empty_dicts(self) -> None:
        """Empty pooled_metrics yields empty output — no KeyError."""
        payload: dict = {"pooled_metrics": {}}
        means, stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert means == {}
        assert stds == {}

    def test_legacy_bare_key_payload_still_works(self) -> None:
        """Synthetic test fixtures using bare keys continue to resolve correctly.

        Ensures backward compatibility: the existing test_corpus_schema_v3.py
        fixture (which uses 'adm2' / 'vif_scale0' / … directly as pooled_metrics
        keys) still passes after the integer_* fix.
        """
        pooled = {
            "vmaf": {"mean": 92.5, "stddev": 0.5},
        }
        bare_values = {
            "adm2": 0.93,
            "vif_scale0": 0.78,
            "vif_scale1": 0.85,
            "vif_scale2": 0.91,
            "vif_scale3": 0.95,
            "motion2": 2.5,
        }
        for name, mu in bare_values.items():
            pooled[name] = {
                "min": mu - 0.1,
                "max": mu + 0.1,
                "mean": mu,
                "stddev": 0.04,
            }
        payload = {"pooled_metrics": pooled}
        means, stds = parse_feature_aggregates(payload, CANONICAL6_FEATURES)
        assert set(means.keys()) == set(CANONICAL6_FEATURES)
        assert means["adm2"] == pytest.approx(0.93)
        assert means["motion2"] == pytest.approx(2.5)
        assert len(stds) == 6
