# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/quality_runner.py
#
# These tests cover the pure-Python logic in QualityRunner and its subclasses:
# score-key helpers, transform/clip logic, _get_aggr_feature_opts_dict, and
# _assert_extension_format.  No binary is invoked.
"""Pytest cases for vmaf.core.quality_runner — pure-Python logic paths."""

from __future__ import annotations

from unittest.mock import MagicMock

import numpy as np
import pytest

from vmaf.core.quality_runner import (
    QualityRunner,
    VmafQualityRunner,
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def _mock_model(
    score_clip: tuple | None = (0.0, 100.0),
    score_transform: dict | None = None,
    appended_info: dict | None = None,
) -> MagicMock:
    """Return a lightweight model mock for transform/clip tests."""
    model = MagicMock()

    def _get_appended_info(key):
        if appended_info is not None and key in appended_info:
            return appended_info[key]
        if key == "score_clip":
            return score_clip
        if key == "score_transform":
            return score_transform
        return None

    model.get_appended_info.side_effect = _get_appended_info
    return model


# ---------------------------------------------------------------------------
# QualityRunner.get_scores_key / get_score_key
# ---------------------------------------------------------------------------


class TestQualityRunnerKeyHelpers:
    """Concrete subclass used only for testing the class-level helpers."""

    class _FakeQR(QualityRunner):
        TYPE = "FAKE_QR"
        VERSION = "1.0"

        def _get_quality_scores(self, asset):
            raise NotImplementedError

        def _generate_result(self, asset):
            raise NotImplementedError

    def test_get_scores_key(self):
        assert self._FakeQR.get_scores_key() == "FAKE_QR_scores"

    def test_get_score_key(self):
        assert self._FakeQR.get_score_key() == "FAKE_QR_score"


# ---------------------------------------------------------------------------
# VmafQualityRunner._assert_extension_format
# ---------------------------------------------------------------------------


class TestAssertExtensionFormat:
    supported = [".pkl", ".json"]

    def test_pkl_accepted(self):
        VmafQualityRunner._assert_extension_format(self.supported, ".pkl")

    def test_json_accepted(self):
        VmafQualityRunner._assert_extension_format(self.supported, ".json")

    def test_pkl_with_suffix_accepted(self):
        VmafQualityRunner._assert_extension_format(self.supported, ".pkl_2160")

    def test_json_with_suffix_accepted(self):
        VmafQualityRunner._assert_extension_format(self.supported, ".json_720")

    def test_unsupported_format_raises(self):
        with pytest.raises(AssertionError, match="supports .pkl or .json"):
            VmafQualityRunner._assert_extension_format(self.supported, ".pkkl")

    def test_another_unsupported_format_raises(self):
        with pytest.raises(AssertionError, match="supports .pkl or .json"):
            VmafQualityRunner._assert_extension_format(self.supported, ".jsson")

    def test_empty_format_raises(self):
        with pytest.raises(AssertionError):
            VmafQualityRunner._assert_extension_format(self.supported, ".onnx")


# ---------------------------------------------------------------------------
# VmafQualityRunner.clip_score
# ---------------------------------------------------------------------------


class TestClipScore:
    def test_clip_within_bounds_unchanged(self):
        model = _mock_model(score_clip=(0.0, 100.0))
        scores = np.array([0.0, 50.0, 100.0])
        clipped = VmafQualityRunner.clip_score(model, scores)
        np.testing.assert_array_equal(clipped, scores)

    def test_clip_below_lb(self):
        model = _mock_model(score_clip=(0.0, 100.0))
        scores = np.array([-10.0, 50.0])
        clipped = VmafQualityRunner.clip_score(model, scores)
        assert clipped[0] == pytest.approx(0.0)

    def test_clip_above_ub(self):
        model = _mock_model(score_clip=(0.0, 100.0))
        scores = np.array([50.0, 110.0])
        clipped = VmafQualityRunner.clip_score(model, scores)
        assert clipped[1] == pytest.approx(100.0)

    def test_no_clip_when_score_clip_is_none(self):
        model = _mock_model(score_clip=None)
        scores = np.array([-999.0, 999.0])
        result = VmafQualityRunner.clip_score(model, scores)
        np.testing.assert_array_equal(result, scores)


# ---------------------------------------------------------------------------
# VmafQualityRunner.transform_score — polynomial branch
# ---------------------------------------------------------------------------


class TestTransformScorePolynomial:
    def test_p0_only_is_constant(self):
        # p0 only: y_out = 0 + p0 = constant regardless of input
        model = _mock_model(score_transform={"p0": 10.0})
        scores = np.array([0.0, 50.0, 100.0])
        out = VmafQualityRunner.transform_score(model, scores)
        np.testing.assert_allclose(out, np.full_like(scores, 10.0))

    def test_p1_only_is_linear_scale(self):
        # p1 only: y_out = 0 + p1 * y_in
        model = _mock_model(score_transform={"p1": 2.0})
        scores = np.array([1.0, 2.0, 3.0])
        out = VmafQualityRunner.transform_score(model, scores)
        np.testing.assert_allclose(out, scores * 2.0)

    def test_p0_p1_combined(self):
        # p0 + p1: y_out = p0 + p1 * y_in
        model = _mock_model(score_transform={"p0": 5.0, "p1": 1.0})
        scores = np.array([10.0])
        out = VmafQualityRunner.transform_score(model, scores)
        assert out[0] == pytest.approx(15.0)  # 5 + 1*10

    def test_p2_quadratic_component(self):
        # p2 only: y_out = 0 + p2 * y^2
        model = _mock_model(score_transform={"p2": 1.0})
        scores = np.array([3.0])
        out = VmafQualityRunner.transform_score(model, scores)
        assert out[0] == pytest.approx(9.0)  # 1 * 3^2

    def test_none_transform_returns_input_unchanged(self):
        model = _mock_model(score_transform=None)
        scores = np.array([42.0])
        out = VmafQualityRunner.transform_score(model, scores)
        np.testing.assert_array_equal(out, scores)


# ---------------------------------------------------------------------------
# VmafQualityRunner.transform_score — rectification branch
# ---------------------------------------------------------------------------


class TestTransformScoreRectification:
    def test_out_lte_in_clamps_output_to_input(self):
        model = _mock_model(score_transform={"p0": 20.0, "out_lte_in": "true"})
        scores = np.array([10.0])
        out = VmafQualityRunner.transform_score(model, scores)
        # p0=20 shifts to 30, but out_lte_in forces output <= input (10)
        assert out[0] == pytest.approx(10.0)

    def test_out_gte_in_clamps_output_to_input(self):
        model = _mock_model(score_transform={"p0": -20.0, "out_gte_in": "true"})
        scores = np.array([10.0])
        out = VmafQualityRunner.transform_score(model, scores)
        # p0=-20 shifts to -10, but out_gte_in forces output >= input (10)
        assert out[0] == pytest.approx(10.0)


# ---------------------------------------------------------------------------
# VmafQualityRunner.transform_score — piecewise-linear branch
# ---------------------------------------------------------------------------


class TestTransformScorePiecewiseLinear:
    def test_knots_identity_mapping(self):
        """Knots at integer identity points must return the same values."""
        knots = [[0, 0], [50, 50], [100, 100]]
        model = _mock_model(score_transform={"knots": knots})
        scores = np.array([0.0, 50.0, 100.0])
        out = VmafQualityRunner.transform_score(model, scores)
        np.testing.assert_allclose(out, scores, atol=1e-6)

    def test_knots_scaling_mapping(self):
        """Knots that double the score over [0, 100] -> [0, 200]."""
        knots = [[0, 0], [50, 100], [100, 200]]
        model = _mock_model(score_transform={"knots": knots})
        scores = np.array([25.0])
        out = VmafQualityRunner.transform_score(model, scores)
        assert out[0] == pytest.approx(50.0, abs=1e-3)


# ---------------------------------------------------------------------------
# VmafQualityRunner._do_transform_score
# ---------------------------------------------------------------------------


class TestDoTransformScore:
    def test_both_none_returns_false(self):
        model = _mock_model(score_transform=None)
        assert VmafQualityRunner._do_transform_score(model, {}) is False

    def test_model_flag_true_no_kwargs_returns_true(self):
        model = _mock_model(score_transform={"enabled": True})
        assert VmafQualityRunner._do_transform_score(model, {}) is True

    def test_model_flag_false_no_kwargs_returns_false(self):
        model = _mock_model(score_transform={"enabled": False})
        assert VmafQualityRunner._do_transform_score(model, {}) is False

    def test_kwargs_flag_true_no_model_flag_returns_true(self):
        model = _mock_model(score_transform=None)
        result = VmafQualityRunner._do_transform_score(model, {"enable_transform_score": True})
        assert result is True

    def test_kwargs_flag_false_no_model_flag_returns_false(self):
        model = _mock_model(score_transform=None)
        result = VmafQualityRunner._do_transform_score(model, {"enable_transform_score": False})
        assert result is False

    def test_either_true_returns_true(self):
        """When both model_flag and kwargs_flag are set, True wins."""
        model = _mock_model(score_transform={"enabled": False})
        result = VmafQualityRunner._do_transform_score(model, {"enable_transform_score": True})
        assert result is True

    def test_both_false_returns_false(self):
        model = _mock_model(score_transform={"enabled": False})
        result = VmafQualityRunner._do_transform_score(model, {"enable_transform_score": False})
        assert result is False


# ---------------------------------------------------------------------------
# VmafQualityRunner._get_aggr_feature_opts_dict_from_atom_feature_opts_dicts
# ---------------------------------------------------------------------------


class TestGetAggrFeatureOptsDictFromAtomFeatureOptsDicts:
    def test_consistent_opts_merged(self):
        atom_feature_names = [
            "VMAF_integer_feature_adm2_score",
            "VMAF_integer_feature_motion2_score",
            "VMAF_integer_feature_vif_scale0_score",
            "VMAF_integer_feature_vif_scale1_score",
            "VMAF_integer_feature_vif_scale2_score",
            "VMAF_integer_feature_vif_scale3_score",
        ]
        atom_feature_opts_dicts = [
            {"adm_enhn_gain_limit": 1.0},
            {},
            {"vif_enhn_gain_limit": 1.0},
            {"vif_enhn_gain_limit": 1.0},
            {"vif_enhn_gain_limit": 1.0},
            {"vif_enhn_gain_limit": 1.0},
        ]
        feature_dict = {
            "VMAF_integer_feature": [
                "vif_scale0",
                "vif_scale1",
                "vif_scale2",
                "vif_scale3",
                "adm2",
                "motion2",
            ]
        }
        result = VmafQualityRunner._get_aggr_feature_opts_dict_from_atom_feature_opts_dicts(
            feature_dict, atom_feature_names, atom_feature_opts_dicts
        )
        assert result == {
            "VMAF_integer_feature": {
                "vif_enhn_gain_limit": 1.0,
                "adm_enhn_gain_limit": 1.0,
            }
        }

    def test_inconsistent_opts_raises_assertion(self):
        atom_feature_names = [
            "VMAF_integer_feature_vif_scale0_score",
            "VMAF_integer_feature_vif_scale1_score",
        ]
        # scale0 and scale1 disagree on vif_enhn_gain_limit
        atom_feature_opts_dicts = [
            {"vif_enhn_gain_limit": 1.0},
            {"vif_enhn_gain_limit": 1.4},
        ]
        feature_dict = {"VMAF_integer_feature": ["vif_scale0", "vif_scale1"]}
        with pytest.raises(AssertionError, match="inconsistent"):
            VmafQualityRunner._get_aggr_feature_opts_dict_from_atom_feature_opts_dicts(
                feature_dict, atom_feature_names, atom_feature_opts_dicts
            )

    def test_empty_opts_produce_empty_inner_dict(self):
        """All opts dicts empty -> the aggregate feature key is present but maps to {}."""
        atom_feature_names = ["VMAF_integer_feature_motion2_score"]
        atom_feature_opts_dicts = [{}]
        feature_dict = {"VMAF_integer_feature": ["motion2"]}
        result = VmafQualityRunner._get_aggr_feature_opts_dict_from_atom_feature_opts_dicts(
            feature_dict, atom_feature_names, atom_feature_opts_dicts
        )
        # The aggregate-feature key is created even when opts are empty (the
        # code initialises the inner dict on first feature hit, then iterates
        # over the per-opt entries — with zero opts the inner dict stays {}).
        assert result == {"VMAF_integer_feature": {}}


# ---------------------------------------------------------------------------
# VmafQualityRunner.predict_with_model — clip-only path
# ---------------------------------------------------------------------------


class TestPredictWithModel:
    def _make_model_and_xs(self, lb=0.0, ub=100.0):
        model = _mock_model(score_clip=(lb, ub), score_transform=None)
        model.predict.return_value = {"ys_label_pred": np.array([50.0, 120.0, -5.0])}
        xs = MagicMock()
        return model, xs

    def test_predict_clips_scores(self):
        model, xs = self._make_model_and_xs()
        result = VmafQualityRunner.predict_with_model(model, xs)
        ys = result["ys_pred"]
        assert ys[0] == pytest.approx(50.0)
        assert ys[1] == pytest.approx(100.0)
        assert ys[2] == pytest.approx(0.0)

    def test_predict_with_disable_clip_skips_clip(self):
        model, xs = self._make_model_and_xs()
        result = VmafQualityRunner.predict_with_model(model, xs, disable_clip_score=True)
        ys = result["ys_pred"]
        # Scores outside [0, 100] are preserved when clipping disabled.
        assert ys[1] == pytest.approx(120.0)
        assert ys[2] == pytest.approx(-5.0)
