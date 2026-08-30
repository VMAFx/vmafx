# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""Smoke tests for Research-0026 feature-set additions.

The Phase-1 deliverable in Research-0026 extends
``ai/data/feature_extractor.py`` with a ``FULL_FEATURES`` tuple +
``FEATURE_SETS`` registry + ``resolve_feature_set`` helper. These
tests cover the registry / resolver only — calling ``extract_features``
itself requires a built libvmaf CLI and is exercised in
``test_netflix_loader.py`` against the canonical 6-feature path.
"""

from __future__ import annotations

import pytest

from ai.data.feature_extractor import (
    _METRIC_TO_EXTRACTOR,
    DEFAULT_FEATURES,
    FEATURE_SETS,
    FULL_FEATURES,
    _extractors_for,
    resolve_feature_set,
)


def test_default_features_unchanged():
    """Canonical 6-feature set must stay byte-identical to vmaf_v0.6.1.

    Regression guard: anything that quietly broadens DEFAULT_FEATURES
    invalidates every shipped tiny-AI ONNX (the input dimension is
    baked into the model).
    """
    assert DEFAULT_FEATURES == (
        "adm2",
        "vif_scale0",
        "vif_scale1",
        "vif_scale2",
        "vif_scale3",
        "motion2",
    )


def test_full_features_superset_of_default():
    assert set(DEFAULT_FEATURES).issubset(set(FULL_FEATURES))


def test_full_features_excludes_lpips_and_moment():
    """Per Research-0026 §"Open questions" Q1: lpips and float_moment
    are deliberately excluded from the v1 candidate pool.
    """
    forbidden = {"lpips", "float_moment", "moment_y", "moment_cb", "moment_cr"}
    assert forbidden.isdisjoint(set(FULL_FEATURES))


def test_full_features_count():
    """Sanity: ~22 features per Research-0026 inventory."""
    assert 18 <= len(FULL_FEATURES) <= 28


def test_every_full_feature_has_extractor_mapping():
    """If a feature is in FULL_FEATURES, _METRIC_TO_EXTRACTOR must
    know which CLI ``--feature`` extractor to invoke. Otherwise the
    libvmaf CLI is asked for an unregistered feature and silently
    returns NaN columns.
    """
    missing = [f for f in FULL_FEATURES if f not in _METRIC_TO_EXTRACTOR]
    assert not missing, f"FULL_FEATURES missing extractor mapping: {missing}"


def test_resolve_feature_set_canonical_and_full():
    assert resolve_feature_set("canonical") == DEFAULT_FEATURES
    assert resolve_feature_set("full") == FULL_FEATURES


def test_resolve_feature_set_unknown_raises():
    with pytest.raises(ValueError, match="unknown feature set"):
        resolve_feature_set("ppx99")


def test_feature_sets_registry_keys():
    assert "canonical" in FEATURE_SETS
    assert "full" in FEATURE_SETS


def test_extractors_for_full_set_dedups():
    """``_extractors_for`` should collapse the FULL set to a small
    number of unique CLI ``--feature`` flags. ADM has 6 metrics, VIF
    5, motion 2, but each maps to one extractor — net unique should
    be roughly the registered-extractor count, not the metric count.
    """
    extractors = _extractors_for(FULL_FEATURES)
    # Expect at most ~12 unique extractors (adm, vif, motion, motion3,
    # psnr, float_ssim, float_ms_ssim, cambi, ciede, psnr_hvs,
    # ssimulacra2). Cap loosely to allow future additions.
    assert 8 <= len(extractors) <= 14, f"got {len(extractors)} extractors: {extractors}"
    assert "adm" in extractors
    assert "vif" in extractors
    assert "motion" in extractors
