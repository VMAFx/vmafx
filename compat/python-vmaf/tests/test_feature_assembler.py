# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/feature_assembler.py
#
# These tests exercise FeatureAssembler construction, _get_atom_features,
# _get_scores_key, and _create_feature_result_dicts without invoking the vmaf
# binary.  All feature-extractor "run" calls are mocked out.
"""Pytest cases for vmaf.core.feature_assembler.FeatureAssembler."""

from __future__ import annotations

from unittest.mock import MagicMock, patch

import pytest

from vmaf.core.feature_assembler import FeatureAssembler
from vmaf.core.feature_extractor import VmafFeatureExtractor
from vmaf.core.result import BasicResult

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _make_asset(name: str = "asset0") -> MagicMock:
    a = MagicMock()
    a.__repr__ = lambda self: name
    return a


def _make_assembler(
    feature_dict: dict,
    assets: list | None = None,
    feature_option_dict: dict | None = None,
) -> FeatureAssembler:
    if assets is None:
        assets = [_make_asset()]
    return FeatureAssembler(
        feature_dict=feature_dict,
        feature_option_dict=feature_option_dict,
        assets=assets,
        logger=None,
        fifo_mode=False,
        delete_workdir=True,
        result_store=None,
        optional_dict=None,
        optional_dict2=None,
        parallelize=False,
    )


# ---------------------------------------------------------------------------
# Construction / attribute assignment
# ---------------------------------------------------------------------------


class TestFeatureAssemblerConstruction:
    def test_stores_feature_dict(self):
        fa = _make_assembler({"VMAF_feature": "all"})
        assert fa.feature_dict == {"VMAF_feature": "all"}

    def test_stores_assets(self):
        assets = [_make_asset("a"), _make_asset("b")]
        fa = _make_assembler({"VMAF_feature": "all"}, assets=assets)
        assert fa.assets is assets

    def test_type2results_dict_initially_empty(self):
        fa = _make_assembler({"VMAF_feature": "all"})
        assert fa.type2results_dict == {}

    def test_optional_dict_propagated(self):
        fa = FeatureAssembler(
            feature_dict={"VMAF_feature": "all"},
            feature_option_dict=None,
            assets=[_make_asset()],
            logger=None,
            fifo_mode=True,
            delete_workdir=True,
            result_store=None,
            optional_dict={"vif_enhn_gain_limit": 1.0},
            optional_dict2=None,
            parallelize=False,
        )
        assert fa.optional_dict == {"vif_enhn_gain_limit": 1.0}

    def test_processes_attribute_set(self):
        fa = FeatureAssembler(
            feature_dict={"VMAF_feature": "all"},
            feature_option_dict=None,
            assets=[_make_asset()],
            logger=None,
            fifo_mode=False,
            delete_workdir=True,
            result_store=None,
            parallelize=True,
            processes=4,
        )
        assert fa.processes == 4


# ---------------------------------------------------------------------------
# _get_atom_features
# ---------------------------------------------------------------------------


class TestGetAtomFeatures:
    def test_all_returns_atom_features_list(self):
        fa = _make_assembler({"VMAF_feature": "all"})
        atom_features = fa._get_atom_features("VMAF_feature")
        # VmafFeatureExtractor.ATOM_FEATURES must be present
        for af in VmafFeatureExtractor.ATOM_FEATURES:
            assert af in atom_features

    def test_all_includes_derived_atom_features(self):
        fa = _make_assembler({"VMAF_feature": "all"})
        atom_features = fa._get_atom_features("VMAF_feature")
        for daf in VmafFeatureExtractor.DERIVED_ATOM_FEATURES:
            assert daf in atom_features

    def test_explicit_list_returned_verbatim(self):
        fa = _make_assembler({"VMAF_feature": ["vif", "adm"]})
        assert fa._get_atom_features("VMAF_feature") == ["vif", "adm"]


# ---------------------------------------------------------------------------
# _get_scores_key
# ---------------------------------------------------------------------------


class TestGetScoresKey:
    def test_vmaf_feature_vif_scores_key(self):
        fa = _make_assembler({"VMAF_feature": ["vif"]})
        key = fa._get_scores_key("VMAF_feature", "vif")
        assert key == VmafFeatureExtractor.get_scores_key("vif")

    def test_key_format(self):
        fa = _make_assembler({"VMAF_feature": ["adm2"]})
        key = fa._get_scores_key("VMAF_feature", "adm2")
        assert key.endswith("_scores")
        assert "adm2" in key


# ---------------------------------------------------------------------------
# _get_fextractor_instance — option-dict routing
# ---------------------------------------------------------------------------


class TestGetFextractorInstance:
    """Verify option-dict routing in _get_fextractor_instance.

    The Executor constructor calls _assert_an_asset which requires ffmpeg to be
    configured.  We patch that assertion out so these unit tests focus solely on
    the optional_dict routing logic — not on ffmpeg availability.
    """

    def _patch_assert(self):
        # Patch out the entire asset-validation routine in Executor.__init__ so
        # these tests focus on option-dict routing without requiring ffmpeg or
        # real YUV files.
        return patch("vmaf.core.executor.Executor._assert_assets", return_value=None)

    def test_uses_optional_dict_when_no_feature_option_dict(self):
        opt = {"vif_enhn_gain_limit": 1.0}
        fa = FeatureAssembler(
            feature_dict={"VMAF_feature": "all"},
            feature_option_dict=None,
            assets=[_make_asset()],
            logger=None,
            fifo_mode=False,
            delete_workdir=True,
            result_store=None,
            optional_dict=opt,
            optional_dict2=None,
            parallelize=False,
        )
        with self._patch_assert():
            extractor = fa._get_fextractor_instance("VMAF_feature")
        assert extractor.optional_dict is opt

    def test_uses_feature_option_dict_over_optional_dict(self):
        per_feature_opt = {"vif_enhn_gain_limit": 2.0}
        global_opt = {"vif_enhn_gain_limit": 1.0}
        fa = FeatureAssembler(
            feature_dict={"VMAF_feature": "all"},
            feature_option_dict={"VMAF_feature": per_feature_opt},
            assets=[_make_asset()],
            logger=None,
            fifo_mode=False,
            delete_workdir=True,
            result_store=None,
            optional_dict=global_opt,
            optional_dict2=None,
            parallelize=False,
        )
        with self._patch_assert():
            extractor = fa._get_fextractor_instance("VMAF_feature")
        assert extractor.optional_dict is per_feature_opt

    def test_falls_back_to_global_opt_when_type_absent_from_feature_option_dict(self):
        global_opt = {"adm_enhn_gain_limit": 1.0}
        fa = FeatureAssembler(
            feature_dict={"VMAF_feature": "all"},
            feature_option_dict={"SOME_OTHER_FEATURE": {}},
            assets=[_make_asset()],
            logger=None,
            fifo_mode=False,
            delete_workdir=True,
            result_store=None,
            optional_dict=global_opt,
            optional_dict2=None,
            parallelize=False,
        )
        with self._patch_assert():
            extractor = fa._get_fextractor_instance("VMAF_feature")
        assert extractor.optional_dict is global_opt


# ---------------------------------------------------------------------------
# _create_feature_result_dicts — assembles results from type2results_dict
# ---------------------------------------------------------------------------


class TestCreateFeatureResultDicts:
    """Test _create_feature_result_dicts without running the binary."""

    def _build_mock_result(self, scores_key: str, scores: list[float]) -> MagicMock:
        """Minimal Result-like mock with result_dict and __getitem__ support."""
        mock_result = MagicMock()
        mock_result.result_dict = {scores_key: scores}
        mock_result.__getitem__ = lambda self, key: self.result_dict[key]
        return mock_result

    def test_single_feature_single_asset(self):
        assets = [_make_asset()]
        fa = _make_assembler({"VMAF_feature": ["vif"]}, assets=assets)

        scores_key = VmafFeatureExtractor.get_scores_key("vif")
        mock_res = self._build_mock_result(scores_key, [0.9, 0.8, 0.85])
        fa.type2results_dict = {"VMAF_feature": [mock_res]}

        result_dicts = fa._create_feature_result_dicts()
        assert len(result_dicts) == 1
        assert scores_key in result_dicts[0]
        assert result_dicts[0][scores_key] == [0.9, 0.8, 0.85]

    def test_multiple_assets(self):
        assets = [_make_asset("a1"), _make_asset("a2")]
        fa = _make_assembler({"VMAF_feature": ["adm2"]}, assets=assets)

        scores_key = VmafFeatureExtractor.get_scores_key("adm2")
        mock_res_0 = self._build_mock_result(scores_key, [0.95])
        mock_res_1 = self._build_mock_result(scores_key, [0.80])
        fa.type2results_dict = {"VMAF_feature": [mock_res_0, mock_res_1]}

        result_dicts = fa._create_feature_result_dicts()
        assert len(result_dicts) == 2
        assert result_dicts[0][scores_key] == [0.95]
        assert result_dicts[1][scores_key] == [0.80]


# ---------------------------------------------------------------------------
# run() integration with mocked extractor
# ---------------------------------------------------------------------------


class TestFeatureAssemblerRun:
    def test_run_populates_results(self):
        """run() must populate self.results with BasicResult objects."""
        assets = [_make_asset()]
        fa = _make_assembler({"VMAF_feature": ["vif"]}, assets=assets)

        scores_key = VmafFeatureExtractor.get_scores_key("vif")

        # Create a minimal stand-in for the FeatureExtractor runner.
        mock_fextractor = MagicMock()
        mock_result = MagicMock()
        mock_result.result_dict = {scores_key: [0.9, 0.8]}
        mock_result.__getitem__ = lambda self, k: self.result_dict[k]
        mock_fextractor.results = [mock_result]

        with patch.object(fa, "_get_fextractor_instance", return_value=mock_fextractor):
            fa.run()

        assert hasattr(fa, "results")
        assert len(fa.results) == 1
        assert isinstance(fa.results[0], BasicResult)

    def test_run_type2results_dict_populated(self):
        assets = [_make_asset()]
        fa = _make_assembler({"VMAF_feature": ["vif"]}, assets=assets)

        scores_key = VmafFeatureExtractor.get_scores_key("vif")
        mock_fextractor = MagicMock()
        mock_result = MagicMock()
        mock_result.result_dict = {scores_key: [0.9]}
        mock_result.__getitem__ = lambda self, k: self.result_dict[k]
        mock_fextractor.results = [mock_result]

        with patch.object(fa, "_get_fextractor_instance", return_value=mock_fextractor):
            fa.run()

        assert "VMAF_feature" in fa.type2results_dict

    def test_run_results_accessible_via_bracket(self):
        assets = [_make_asset()]
        fa = _make_assembler({"VMAF_feature": ["vif"]}, assets=assets)

        scores_key = VmafFeatureExtractor.get_scores_key("vif")
        mock_fextractor = MagicMock()
        mock_result = MagicMock()
        scores_value = [0.91, 0.88, 0.93]
        mock_result.result_dict = {scores_key: scores_value}
        mock_result.__getitem__ = lambda self, k: self.result_dict[k]
        mock_fextractor.results = [mock_result]

        with patch.object(fa, "_get_fextractor_instance", return_value=mock_fextractor):
            fa.run()

        # BasicResult supports dictionary-like access for aggregate score.
        br = fa.results[0]
        assert br.result_dict[scores_key] == scores_value
