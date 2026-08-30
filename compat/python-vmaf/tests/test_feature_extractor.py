# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/feature_extractor.py
#
# These tests cover the class-level helpers and metadata on FeatureExtractor
# and its concrete subclasses without invoking the vmaf binary.
"""Pytest cases for vmaf.core.feature_extractor — class metadata and helpers."""

from __future__ import annotations

import pytest

from vmaf.core.feature_extractor import (
    FeatureExtractor,
    MomentFeatureExtractor,
    MsSsimFeatureExtractor,
    PsnrFeatureExtractor,
    PyPsnrFeatureExtractor,
    SpeedChromaFeatureExtractor,
    SpeedTemporalFeatureExtractor,
    SsimFeatureExtractor,
    VifFrameDifferenceFeatureExtractor,
    VmafFeatureExtractor,
    VmafIntegerFeatureExtractor,
)

# ---------------------------------------------------------------------------
# FeatureExtractor.get_scores_key / get_score_key
# ---------------------------------------------------------------------------


class TestFeatureExtractorKeyHelpers:
    def test_vmaf_feature_get_scores_key_vif(self):
        key = VmafFeatureExtractor.get_scores_key("vif")
        assert key == "VMAF_feature_vif_scores"

    def test_vmaf_feature_get_score_key_adm2(self):
        key = VmafFeatureExtractor.get_score_key("adm2")
        assert key == "VMAF_feature_adm2_score"

    def test_vmaf_integer_feature_get_scores_key_motion2(self):
        key = VmafIntegerFeatureExtractor.get_scores_key("motion2")
        assert key == "VMAF_integer_feature_motion2_scores"

    def test_vmaf_integer_feature_get_score_key_vif_scale0(self):
        key = VmafIntegerFeatureExtractor.get_score_key("vif_scale0")
        assert key == "VMAF_integer_feature_vif_scale0_score"

    def test_psnr_feature_get_scores_key(self):
        key = PsnrFeatureExtractor.get_scores_key("psnr")
        assert key == "PSNR_feature_psnr_scores"

    def test_ssim_feature_get_scores_key(self):
        key = SsimFeatureExtractor.get_scores_key("ssim")
        assert key == "SSIM_feature_ssim_scores"

    def test_ms_ssim_feature_get_scores_key(self):
        key = MsSsimFeatureExtractor.get_scores_key("ms_ssim")
        assert key == "MS_SSIM_feature_ms_ssim_scores"

    def test_moment_feature_get_scores_key_ref1st(self):
        key = MomentFeatureExtractor.get_scores_key("ref1st")
        assert key == "Moment_feature_ref1st_scores"

    def test_speed_chroma_feature_get_scores_key(self):
        key = SpeedChromaFeatureExtractor.get_scores_key("speed_chroma_uv")
        assert key == "Speed_chroma_feature_speed_chroma_uv_scores"

    def test_speed_temporal_feature_get_scores_key(self):
        key = SpeedTemporalFeatureExtractor.get_scores_key("speed_temporal")
        assert key == "Speed_temporal_feature_speed_temporal_scores"


# ---------------------------------------------------------------------------
# ATOM_FEATURES membership
# ---------------------------------------------------------------------------


class TestAtomFeaturesMembership:
    def test_vmaf_feature_contains_vif(self):
        assert "vif" in VmafFeatureExtractor.ATOM_FEATURES

    def test_vmaf_feature_contains_adm(self):
        assert "adm" in VmafFeatureExtractor.ATOM_FEATURES

    def test_vmaf_feature_contains_motion(self):
        assert "motion" in VmafFeatureExtractor.ATOM_FEATURES

    def test_vmaf_feature_contains_adm2(self):
        assert "adm2" in VmafFeatureExtractor.ATOM_FEATURES

    def test_vmaf_feature_contains_vif_scale0_through_3(self):
        for i in range(4):
            assert f"vif_scale{i}" in VmafFeatureExtractor.ATOM_FEATURES

    def test_vmaf_integer_feature_shares_atom_features_with_float(self):
        """VmafIntegerFeatureExtractor inherits ATOM_FEATURES from VmafFeatureExtractor."""
        assert VmafIntegerFeatureExtractor.ATOM_FEATURES is VmafFeatureExtractor.ATOM_FEATURES

    def test_ssim_feature_contains_ssim_lcs_components(self):
        for feature in ["ssim", "ssim_l", "ssim_c", "ssim_s"]:
            assert feature in SsimFeatureExtractor.ATOM_FEATURES

    def test_psnr_feature_has_psnr(self):
        assert "psnr" in PsnrFeatureExtractor.ATOM_FEATURES

    def test_moment_feature_has_four_atoms(self):
        for atom in ["ref1st", "ref2nd", "dis1st", "dis2nd"]:
            assert atom in MomentFeatureExtractor.ATOM_FEATURES

    def test_speed_chroma_has_three_atoms(self):
        for atom in ["speed_chroma_u", "speed_chroma_v", "speed_chroma_uv"]:
            assert atom in SpeedChromaFeatureExtractor.ATOM_FEATURES

    def test_speed_temporal_has_one_atom(self):
        assert "speed_temporal" in SpeedTemporalFeatureExtractor.ATOM_FEATURES

    def test_vif_frame_diff_has_vifdiff_atom(self):
        assert "vifdiff" in VifFrameDifferenceFeatureExtractor.ATOM_FEATURES


# ---------------------------------------------------------------------------
# DERIVED_ATOM_FEATURES
# ---------------------------------------------------------------------------


class TestDerivedAtomFeatures:
    def test_vmaf_feature_derived_contains_vif2(self):
        assert "vif2" in VmafFeatureExtractor.DERIVED_ATOM_FEATURES

    def test_moment_feature_derived_contains_refvar_disvar(self):
        assert "refvar" in MomentFeatureExtractor.DERIVED_ATOM_FEATURES
        assert "disvar" in MomentFeatureExtractor.DERIVED_ATOM_FEATURES

    def test_vif_frame_diff_derived_contains_vifdiff_scales(self):
        for i in range(4):
            assert f"vifdiff_scale{i}" in VifFrameDifferenceFeatureExtractor.DERIVED_ATOM_FEATURES

    def test_speed_chroma_derived_is_empty(self):
        assert SpeedChromaFeatureExtractor.DERIVED_ATOM_FEATURES == []

    def test_speed_temporal_derived_is_empty(self):
        assert SpeedTemporalFeatureExtractor.DERIVED_ATOM_FEATURES == []


# ---------------------------------------------------------------------------
# TYPE / VERSION attributes
# ---------------------------------------------------------------------------


class TestTypeAndVersion:
    def test_vmaf_feature_type(self):
        assert VmafFeatureExtractor.TYPE == "VMAF_feature"

    def test_vmaf_integer_feature_type(self):
        assert VmafIntegerFeatureExtractor.TYPE == "VMAF_integer_feature"

    def test_psnr_feature_type(self):
        assert PsnrFeatureExtractor.TYPE == "PSNR_feature"

    def test_ssim_feature_type(self):
        assert SsimFeatureExtractor.TYPE == "SSIM_feature"

    def test_ms_ssim_feature_type(self):
        assert MsSsimFeatureExtractor.TYPE == "MS_SSIM_feature"

    def test_moment_feature_type(self):
        assert MomentFeatureExtractor.TYPE == "Moment_feature"

    def test_speed_chroma_feature_type(self):
        assert SpeedChromaFeatureExtractor.TYPE == "Speed_chroma_feature"

    def test_speed_temporal_feature_type(self):
        assert SpeedTemporalFeatureExtractor.TYPE == "Speed_temporal_feature"

    def test_py_psnr_feature_type(self):
        assert PyPsnrFeatureExtractor.TYPE == "PyPsnr_feature"

    def test_vif_frame_diff_type(self):
        assert VifFrameDifferenceFeatureExtractor.TYPE == "VifDiff_feature"

    def test_all_extractors_have_version_string(self):
        for cls in [
            VmafFeatureExtractor,
            VmafIntegerFeatureExtractor,
            PsnrFeatureExtractor,
            SsimFeatureExtractor,
            MsSsimFeatureExtractor,
            MomentFeatureExtractor,
            SpeedChromaFeatureExtractor,
            SpeedTemporalFeatureExtractor,
            PyPsnrFeatureExtractor,
        ]:
            assert isinstance(cls.VERSION, str) and len(cls.VERSION) > 0


# ---------------------------------------------------------------------------
# ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT
# ---------------------------------------------------------------------------


class TestAtomFeaturesToVmafexecKeyDict:
    def test_vmaf_feature_identity_mapping(self):
        """For VmafFeatureExtractor all atom features map to themselves."""
        d = VmafFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT
        for af in VmafFeatureExtractor.ATOM_FEATURES:
            assert af in d
            assert d[af] == af

    def test_vmaf_integer_feature_prefixes_with_integer(self):
        d = VmafIntegerFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT
        for af in VmafFeatureExtractor.ATOM_FEATURES:
            assert af in d
            assert d[af] == f"integer_{af}"

    def test_psnr_feature_maps_psnr_to_float_psnr(self):
        assert PsnrFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT["psnr"] == "float_psnr"

    def test_ssim_feature_maps_ssim_to_float_ssim(self):
        assert SsimFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT["ssim"] == "float_ssim"

    def test_speed_chroma_identity_mapping(self):
        d = SpeedChromaFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT
        assert d["speed_chroma_uv"] == "speed_chroma_uv"
        assert d["speed_chroma_u"] == "speed_chroma_u"
        assert d["speed_chroma_v"] == "speed_chroma_v"

    def test_speed_temporal_identity_mapping(self):
        d = SpeedTemporalFeatureExtractor.ATOM_FEATURES_TO_VMAFEXEC_KEY_DICT
        assert d["speed_temporal"] == "speed_temporal"


# ---------------------------------------------------------------------------
# FeatureExtractor.find_subclass
# ---------------------------------------------------------------------------


class TestFindSubclass:
    def test_find_vmaf_feature(self):
        cls = FeatureExtractor.find_subclass("VMAF_feature")
        assert cls is VmafFeatureExtractor

    def test_find_vmaf_integer_feature(self):
        cls = FeatureExtractor.find_subclass("VMAF_integer_feature")
        assert cls is VmafIntegerFeatureExtractor

    def test_find_psnr_feature(self):
        cls = FeatureExtractor.find_subclass("PSNR_feature")
        assert cls is PsnrFeatureExtractor

    def test_find_ssim_feature(self):
        cls = FeatureExtractor.find_subclass("SSIM_feature")
        assert cls is SsimFeatureExtractor

    def test_find_ms_ssim_feature(self):
        cls = FeatureExtractor.find_subclass("MS_SSIM_feature")
        assert cls is MsSsimFeatureExtractor

    def test_find_moment_feature(self):
        cls = FeatureExtractor.find_subclass("Moment_feature")
        assert cls is MomentFeatureExtractor

    def test_find_speed_chroma_feature(self):
        cls = FeatureExtractor.find_subclass("Speed_chroma_feature")
        assert cls is SpeedChromaFeatureExtractor

    def test_find_speed_temporal_feature(self):
        cls = FeatureExtractor.find_subclass("Speed_temporal_feature")
        assert cls is SpeedTemporalFeatureExtractor

    def test_find_unknown_raises(self):
        with pytest.raises(Exception):
            FeatureExtractor.find_subclass("NONEXISTENT_feature")

    def test_get_subclasses_recursively_includes_concrete_classes(self):
        subclasses = FeatureExtractor.get_subclasses_recursively()
        assert VmafFeatureExtractor in subclasses
        assert VmafIntegerFeatureExtractor in subclasses
        assert PsnrFeatureExtractor in subclasses
        assert SsimFeatureExtractor in subclasses
        assert MomentFeatureExtractor in subclasses


# ---------------------------------------------------------------------------
# VmafexecFeatureExtractorMixin._discover_feature_exact / _wildcard
# ---------------------------------------------------------------------------


class TestDiscoverFeatureExact:
    """Test the exact-match discovery helper without running the binary."""

    def _make_frame(self, attribs: dict):
        """Minimal XML-frame-like mock."""
        from unittest.mock import MagicMock

        frame = MagicMock()
        frame.attrib = attribs
        return frame

    def test_exact_match_found(self):
        frame = self._make_frame({"float_psnr": "42.5", "frameNum": "0"})
        feature_scores = [[]]
        feature_nicknames = [None]
        found = PsnrFeatureExtractor._discover_feature_exact(
            frame, 0, "psnr", feature_scores, feature_nicknames
        )
        assert found is True
        assert feature_scores[0] == [42.5]
        assert feature_nicknames[0] == "psnr"

    def test_exact_match_not_found(self):
        frame = self._make_frame({"some_other": "1.0"})
        feature_scores = [[]]
        feature_nicknames = [None]
        found = PsnrFeatureExtractor._discover_feature_exact(
            frame, 0, "psnr", feature_scores, feature_nicknames
        )
        assert found is False
        assert feature_scores[0] == []

    def test_exact_match_consistent_nickname(self):
        """Calling twice with same feature must not change the nickname."""
        frame = self._make_frame({"float_psnr": "35.0"})
        feature_scores = [[], []]
        feature_nicknames = ["psnr", None]
        PsnrFeatureExtractor._discover_feature_exact(
            frame, 0, "psnr", feature_scores, feature_nicknames
        )
        assert feature_nicknames[0] == "psnr"


class TestDiscoverFeatureWildcard:
    def _make_frame(self, attribs: dict):
        from unittest.mock import MagicMock

        frame = MagicMock()
        frame.attrib = attribs
        return frame

    def test_wildcard_shortest_wins(self):
        """Wildcard picks the shortest suffix among multiple wildcard candidates.

        The prefix for vif_scale0 under VmafIntegerFeatureExtractor is
        ``integer_vif_scale0_``.  Two candidates both start with that prefix;
        the shorter one is selected.
        """
        frame = self._make_frame(
            {
                "integer_vif_scale0_egl_1": "0.9",
                "integer_vif_scale0_eg_1": "0.95",
            }
        )
        feature_scores = [[]]
        feature_nicknames = [None]
        found = VmafIntegerFeatureExtractor._discover_feature_wildcard(
            frame, 0, "vif_scale0", feature_scores, feature_nicknames
        )
        assert found is True
        # Shorter match "integer_vif_scale0_eg_1" wins.
        assert feature_scores[0] == [pytest.approx(0.95)]

    def test_wildcard_not_found(self):
        frame = self._make_frame({"unrelated_key": "1.0"})
        feature_scores = [[]]
        feature_nicknames = [None]
        found = VmafIntegerFeatureExtractor._discover_feature_wildcard(
            frame, 0, "vif_scale0", feature_scores, feature_nicknames
        )
        assert found is False
