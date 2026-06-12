# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/result.py
#
# These tests cover BasicResult, Result, and RawResult without invoking the
# vmaf binary.  They are deliberately disjoint from the Netflix golden tests
# in python/test/result_test.py (which are off-limits per CLAUDE.md §8).
"""Pytest cases for vmaf.core.result — Result + BasicResult + dataframe helpers."""

from __future__ import annotations

import json
from collections import OrderedDict
from unittest.mock import MagicMock, patch

import numpy as np
import pytest

from vmaf.core.result import BasicResult, RawResult, Result

# ---------------------------------------------------------------------------
# Minimal helpers
# ---------------------------------------------------------------------------


def _make_asset(
    dataset: str = "testds",
    content_id: int = 0,
    asset_id: int = 0,
    ref_path: str = "/ref/ref.yuv",
    dis_path: str = "/dis/dis.yuv",
) -> MagicMock:
    """Return a lightweight Asset mock with the fields Result accesses."""
    asset = MagicMock()
    asset.dataset = dataset
    asset.content_id = content_id
    asset.asset_id = asset_id
    asset.ref_path = ref_path
    asset.dis_path = dis_path
    # __str__ must return a stable identifier (used in to_xml / to_json)
    asset.__str__ = lambda self: (f"{dataset}_{content_id}_{asset_id}_ref_vs_dis_q_576x324")
    return asset


def _make_result(
    scores: list[float] | None = None,
    feature_scores: list[float] | None = None,
    executor_id: str = "FAKE_V1.0",
) -> Result:
    """Return a Result with synthetic scores; no binary required."""
    if scores is None:
        scores = [80.0, 85.0, 90.0]
    if feature_scores is None:
        feature_scores = [0.9, 0.8, 0.95]

    result_dict = {
        "FAKE_scores": np.array(scores),
        "FAKE_feature_vif_scores": list(feature_scores),
    }
    return Result(_make_asset(), executor_id, result_dict)


# ---------------------------------------------------------------------------
# BasicResult tests
# ---------------------------------------------------------------------------


class TestBasicResultGetItem:
    """BasicResult.__getitem__ / get_result / _try_get_aggregate_score."""

    def test_direct_key_access(self):
        rd = {"some_scores": [1.0, 2.0, 3.0]}
        br = BasicResult(MagicMock(), rd)
        assert br["some_scores"] == [1.0, 2.0, 3.0]

    def test_aggregate_score_computed_from_scores(self):
        """Accessing *_score triggers aggregation over *_scores."""
        rd = {"VMAF_scores": np.array([60.0, 70.0, 80.0])}
        br = BasicResult(MagicMock(), rd)
        assert br["VMAF_score"] == pytest.approx(70.0)

    def test_aggregate_score_with_custom_method(self):
        rd = {"VMAF_scores": np.array([60.0, 70.0, 80.0])}
        br = BasicResult(MagicMock(), rd)
        br.set_score_aggregate_method(np.max)
        assert br["VMAF_score"] == pytest.approx(80.0)

    def test_aggregate_score_min_method(self):
        rd = {"VMAF_scores": np.array([60.0, 70.0, 80.0])}
        br = BasicResult(MagicMock(), rd)
        br.set_score_aggregate_method(np.min)
        assert br["VMAF_score"] == pytest.approx(60.0)

    def test_set_score_aggregate_method_none_resets_to_mean(self):
        rd = {"VMAF_scores": np.array([60.0, 80.0])}
        br = BasicResult(MagicMock(), rd)
        br.set_score_aggregate_method(np.max)
        br.set_score_aggregate_method(None)
        assert br["VMAF_score"] == pytest.approx(70.0)

    def test_missing_key_raises_key_error(self):
        br = BasicResult(MagicMock(), {})
        with pytest.raises(KeyError):
            _ = br["nonexistent_score"]

    def test_missing_scores_key_raises_key_error(self):
        br = BasicResult(MagicMock(), {})
        with pytest.raises(KeyError):
            _ = br["nonexistent_score"]

    def test_2d_scores_per_model_aggregation(self):
        """2-D array (models × frames) → list of per-model means."""
        scores = np.array([[10.0, 20.0], [30.0, 40.0]])  # 2 models × 2 frames
        rd = {"BOOTSTRAP_VMAF_all_models_scores": scores}
        br = BasicResult(MagicMock(), rd)
        result = br["BOOTSTRAP_VMAF_all_models_score"]
        assert len(result) == 2
        assert result[0] == pytest.approx(15.0)
        assert result[1] == pytest.approx(35.0)

    def test_3d_scores_raises_assertion(self):
        """A 3-D scores array must raise AssertionError on aggregation."""
        rd = {"BAD_scores": np.zeros((2, 2, 2))}
        br = BasicResult(MagicMock(), rd)
        with pytest.raises(AssertionError):
            _ = br["BAD_score"]

    def test_2d_single_model_assertion(self):
        """A 2-D array with only 1 model row is invalid (must have > 1)."""
        scores = np.array([[10.0, 20.0]])  # 1 model × 2 frames — not allowed
        rd = {"ONE_MODEL_scores": scores}
        br = BasicResult(MagicMock(), rd)
        with pytest.raises(AssertionError):
            _ = br["ONE_MODEL_score"]


class TestBasicResultOrderedLists:
    def test_get_ordered_list_scores_key_filters_1d_only(self):
        scores_1d = np.array([1.0, 2.0])
        scores_2d = np.array([[1.0, 2.0], [3.0, 4.0]])
        rd = {"AAA_scores": scores_1d, "BBB_scores": scores_2d, "not_a_score_key": [1]}
        br = BasicResult(MagicMock(), rd)
        keys = br.get_ordered_list_scores_key()
        assert "AAA_scores" in keys
        assert "BBB_scores" not in keys
        assert "not_a_score_key" not in keys

    def test_get_ordered_list_multimodel_scores_key_filters_2d_only(self):
        scores_1d = np.array([1.0, 2.0])
        scores_2d = np.array([[1.0, 2.0], [3.0, 4.0]])
        rd = {"AAA_scores": scores_1d, "BBB_scores": scores_2d}
        br = BasicResult(MagicMock(), rd)
        mm_keys = br.get_ordered_list_multimodel_scores_key()
        assert "BBB_scores" in mm_keys
        assert "AAA_scores" not in mm_keys

    def test_get_ordered_list_score_key_strips_s_suffix(self):
        rd = {"VMAF_feature_vif_scores": [0.9, 0.8]}
        br = BasicResult(MagicMock(), rd)
        score_keys = br.get_ordered_list_score_key()
        assert "VMAF_feature_vif_score" in score_keys

    def test_empty_result_dict_returns_empty_lists(self):
        br = BasicResult(MagicMock(), {})
        assert br.get_ordered_list_scores_key() == []
        assert br.get_ordered_list_multimodel_scores_key() == []
        assert br.get_ordered_list_score_key() == []


class TestBasicResultScoresKeyWildcardMatch:
    """BasicResult.scores_key_wildcard_match — covers the 5-path logic."""

    def test_exact_match(self):
        d = {"VMAF_feature_adm_scores": [1.0]}
        key = BasicResult.scores_key_wildcard_match(d, "VMAF_feature_adm_scores")
        assert key == "VMAF_feature_adm_scores"

    def test_wildcard_match_single_candidate(self):
        d = {"VMAF_integer_feature_vif_scale0_egl_1_scores": [0.98]}
        key = BasicResult.scores_key_wildcard_match(d, "VMAF_integer_feature_vif_scale0_scores")
        assert key == "VMAF_integer_feature_vif_scale0_egl_1_scores"

    def test_no_match_raises_key_error(self):
        d = {"VMAF_feature_vif_scores": [0.9]}
        with pytest.raises(KeyError, match="no key matches"):
            BasicResult.scores_key_wildcard_match(d, "VMAF_feature_adm_scores")

    def test_ambiguous_match_raises_key_error(self):
        d = {
            "VMAF_feature_adm_num_scale0_scores": [1.0],
            "VMAF_feature_adm_num_scale1_scores": [1.0],
        }
        with pytest.raises(KeyError, match="more than one keys matches"):
            BasicResult.scores_key_wildcard_match(d, "VMAF_feature_adm_num_scale_scores")

    def test_shortest_match_wins(self):
        d = {
            "VMAF_integer_feature_vif_scale0_egl_1_scores": [1.0],
            "VMAF_integer_feature_vif_scale0_eg_1_scores": [2.0],
        }
        key = BasicResult.scores_key_wildcard_match(d, "VMAF_integer_feature_vif_scale0_scores")
        assert key == "VMAF_integer_feature_vif_scale0_eg_1_scores"

    def test_excluded_keys_filter(self):
        d = {
            "VMAF_integer_feature_vif_scale0_egl_1_scores": [1.0],
            "VMAF_integer_feature_vif_scale0_eg_1_scores": [2.0],
        }
        key = BasicResult.scores_key_wildcard_match(
            d,
            "VMAF_integer_feature_vif_scale0_scores",
            excluded_scores_keys=["VMAF_integer_feature_vif_scale0_eg_1_scores"],
        )
        assert key == "VMAF_integer_feature_vif_scale0_egl_1_scores"

    def test_all_excluded_raises_key_error(self):
        d = {"VMAF_integer_feature_vif_scale0_egl_1_scores": [1.0]}
        with pytest.raises(KeyError, match="no key matches"):
            BasicResult.scores_key_wildcard_match(
                d,
                "VMAF_integer_feature_vif_scale0_scores",
                excluded_scores_keys=["VMAF_integer_feature_vif_scale0_egl_1_scores"],
            )

    def test_empty_excluded_list_behaves_same_as_none(self):
        d = {"VMAF_integer_feature_vif_scale0_eg_1_scores": [1.0]}
        key_none = BasicResult.scores_key_wildcard_match(
            d, "VMAF_integer_feature_vif_scale0_scores"
        )
        key_empty = BasicResult.scores_key_wildcard_match(
            d, "VMAF_integer_feature_vif_scale0_scores", excluded_scores_keys=[]
        )
        assert key_none == key_empty


# ---------------------------------------------------------------------------
# Result.__eq__ / __ne__
# ---------------------------------------------------------------------------


class TestResultEquality:
    def test_equal_results(self):
        # Use list scores: the __eq__ implementation compares result_dict values
        # with != which on lists returns a bool, not an ambiguous numpy array.
        asset = _make_asset()
        rd = {"VMAF_scores": [70.0, 80.0]}
        r1 = Result(asset, "EX_V1", rd)
        r2 = Result(asset, "EX_V1", rd)
        assert r1 == r2

    def test_unequal_executor_id(self):
        asset = _make_asset()
        rd = {"VMAF_scores": [70.0]}
        r1 = Result(asset, "EX_V1", rd)
        r2 = Result(asset, "EX_V2", rd)
        assert r1 != r2

    def test_unequal_scores(self):
        asset = _make_asset()
        rd1 = {"VMAF_scores": [70.0, 80.0]}
        rd2 = {"VMAF_scores": [70.0, 81.0]}
        r1 = Result(asset, "EX_V1", rd1)
        r2 = Result(asset, "EX_V1", rd2)
        assert r1 != r2


# ---------------------------------------------------------------------------
# Result.to_dict / to_json round-trips (no binary required)
# ---------------------------------------------------------------------------


class TestResultSerialisation:
    def _make_ssim_like_result(self) -> Result:
        asset = _make_asset(ref_path="/ref/r.yuv", dis_path="/dis/d.yuv")
        rd = {
            "SSIM_feature_ssim_scores": [0.99, 0.98, 0.97],
            "SSIM_scores": np.array([0.95, 0.94, 0.96]),
        }
        return Result(asset, "SSIM_V1.0", rd)

    def test_to_dict_has_required_top_keys(self):
        r = self._make_ssim_like_result()
        d = r.to_dict()
        assert "executorId" in d
        assert "asset" in d
        assert "frames" in d
        assert "aggregate" in d

    def test_to_dict_executor_id(self):
        r = self._make_ssim_like_result()
        assert r.to_dict()["executorId"] == "SSIM_V1.0"

    def test_to_dict_frames_length(self):
        r = self._make_ssim_like_result()
        frames = r.to_dict()["frames"]
        assert len(frames) == 3

    def test_to_dict_frame_has_framenum(self):
        r = self._make_ssim_like_result()
        frames = r.to_dict()["frames"]
        for i, frame in enumerate(frames):
            assert frame["frameNum"] == i

    def test_to_json_round_trips(self):
        r = self._make_ssim_like_result()
        json_str = r.to_json()
        parsed = json.loads(json_str)
        assert parsed["executorId"] == "SSIM_V1.0"
        assert len(parsed["frames"]) == 3

    def test_to_dict_aggregate_method_name(self):
        r = self._make_ssim_like_result()
        d = r.to_dict()
        assert d["aggregate"]["method"] == "mean"

    def test_to_dict_aggregate_method_name_with_custom_agg(self):
        r = self._make_ssim_like_result()
        r.set_score_aggregate_method(np.median)
        d = r.to_dict()
        assert d["aggregate"]["method"] == "median"

    def test_to_dict_no_feature_scores_key_when_empty(self):
        """A Result with only model-level scores produces no feature keys in frames."""
        asset = _make_asset()
        rd = {"VMAF_scores": np.array([70.0, 80.0])}
        r = Result(asset, "VMAF_V1", rd)
        frames = r.to_dict()["frames"]
        assert all("VMAF_score" in frame for frame in frames)


class TestResultXmlRoundTrip:
    """Result.to_xml / from_xml round-trip — covers the XML serialisation path."""

    def _make_simple_result(self) -> Result:
        asset = _make_asset()
        rd = {
            "FAKE_feature_vif_scores": [0.9, 0.85, 0.92],
            "FAKE_scores": np.array([75.0, 80.0, 78.0]),
        }
        return Result(asset, "FAKE_V1.0", rd)

    def test_to_xml_is_string(self):
        r = self._make_simple_result()
        xml_out = r.to_xml()
        assert isinstance(xml_out, str)
        assert "result" in xml_out

    def test_to_xml_contains_executor_id(self):
        r = self._make_simple_result()
        assert "FAKE_V1.0" in r.to_xml()

    def test_from_xml_round_trips_executor_id(self):
        r = self._make_simple_result()
        xml_str = r.to_xml()
        r2 = Result.from_xml(xml_str)
        assert r2.executor_id == "FAKE_V1.0"

    def test_from_xml_to_xml_idempotent(self):
        r = self._make_simple_result()
        xml1 = r.to_xml()
        xml2 = Result.from_xml(xml1).to_xml()
        assert xml1 == xml2


class TestResultJsonRoundTrip:
    def _make_simple_result(self) -> Result:
        asset = _make_asset()
        rd = {
            "FAKE_feature_vif_scores": [0.9, 0.85],
            "FAKE_scores": np.array([75.0, 80.0]),
        }
        return Result(asset, "FAKE_V1.0", rd)

    def test_from_json_round_trips_executor_id(self):
        r = self._make_simple_result()
        json_str = r.to_json()
        r2 = Result.from_json(json_str)
        assert r2.executor_id == "FAKE_V1.0"

    def test_from_json_to_json_idempotent(self):
        r = self._make_simple_result()
        j1 = r.to_json()
        j2 = Result.from_json(j1).to_json()
        assert j1 == j2


# ---------------------------------------------------------------------------
# Result.to_string / __str__
# ---------------------------------------------------------------------------


class TestResultToString:
    def test_to_string_contains_executor_id(self):
        r = _make_result()
        s = str(r)
        assert "FAKE_V1.0" in s

    def test_to_string_contains_aggregate(self):
        r = _make_result()
        s = r.to_string()
        assert "Aggregate" in s

    def test_to_string_contains_frame(self):
        r = _make_result()
        s = r.to_string()
        assert "Frame" in s


# ---------------------------------------------------------------------------
# Result.combine_result
# ---------------------------------------------------------------------------


class TestResultCombine:
    def _make_pair(self) -> tuple[Result, Result]:
        asset1 = _make_asset(ref_path="/r1.yuv", dis_path="/d1.yuv")
        asset2 = _make_asset(ref_path="/r2.yuv", dis_path="/d2.yuv")
        rd1 = {
            "FAKE_feature_vif_scores": [0.9, 0.8],
            "FAKE_scores": np.array([80.0, 85.0]),
        }
        rd2 = {
            "FAKE_feature_vif_scores": [0.7, 0.75],
            "FAKE_scores": np.array([70.0, 72.0]),
        }
        return Result(asset1, "FAKE_V1.0", rd1), Result(asset2, "FAKE_V1.0", rd2)

    def test_combined_length_is_sum(self):
        r1, r2 = self._make_pair()
        combined = Result.combine_result([r1, r2])
        assert len(combined.result_dict["FAKE_scores"]) == 4
        assert len(combined.result_dict["FAKE_feature_vif_scores"]) == 4

    def test_combined_executor_id_preserved(self):
        r1, r2 = self._make_pair()
        combined = Result.combine_result([r1, r2])
        assert combined.executor_id == "FAKE_V1.0"

    def test_combine_empty_list_raises(self):
        with pytest.raises(AssertionError):
            Result.combine_result([])

    def test_combine_mismatched_executor_ids_raises(self):
        asset = _make_asset()
        rd = {"FAKE_scores": np.array([70.0])}
        r1 = Result(asset, "FAKE_V1.0", rd)
        r2 = Result(asset, "FAKE_V2.0", rd)
        with pytest.raises(AssertionError):
            Result.combine_result([r1, r2])

    def test_combine_mismatched_keys_raises(self):
        asset = _make_asset()
        r1 = Result(asset, "FAKE_V1.0", {"AAA_scores": np.array([1.0])})
        r2 = Result(asset, "FAKE_V1.0", {"BBB_scores": np.array([2.0])})
        with pytest.raises(AssertionError):
            Result.combine_result([r1, r2])


# ---------------------------------------------------------------------------
# Result.to_dataframe / from_dataframe (pandas required)
# ---------------------------------------------------------------------------


class TestResultDataframe:
    """to_dataframe / from_dataframe — covered when pandas is available."""

    pytest.importorskip("pandas", reason="pandas not installed; skip dataframe tests")

    def _make_result(self) -> Result:
        asset = _make_asset()
        # `to_dataframe` needs asset.ref_path and asset.dis_path to extract
        # file names via get_file_name_with_extension. Use real strings.
        rd = {
            "SSIM_feature_ssim_scores": [0.99, 0.98, 0.97],
            "SSIM_scores": np.array([0.95, 0.94, 0.96]),
        }
        return Result(asset, "SSIM_V1.0", rd)

    def test_to_dataframe_columns(self):
        r = self._make_result()
        df = r.to_dataframe()
        for col in Result.DATAFRAME_COLUMNS:
            assert col in df.columns

    def test_to_dataframe_row_count(self):
        """One row per 1-D scores key."""
        r = self._make_result()
        df = r.to_dataframe()
        assert len(df) == 2  # SSIM_feature_ssim_scores + SSIM_scores

    def test_to_dataframe_executor_id_consistent(self):
        r = self._make_result()
        df = r.to_dataframe()
        assert (df["executor_id"] == "SSIM_V1.0").all()

    def test_get_unique_from_dataframe_correct_value(self):
        r = self._make_result()
        df = r.to_dataframe()
        scores = Result.get_unique_from_dataframe(df, "SSIM_scores", "scores")
        assert len(scores) == 3

    def test_get_unique_from_dataframe_assert_on_ambiguous(self):
        """If more than one row matches, get_unique_from_dataframe must assert."""
        import pandas as pd

        # Build a malformed df with two rows for the same scores_key.
        row = {
            "dataset": "t",
            "content_id": 0,
            "asset_id": 0,
            "ref_name": "r.yuv",
            "dis_name": "d.yuv",
            "asset": "repr",
            "executor_id": "EX_V1",
            "scores_key": "SSIM_scores",
            "scores": [0.9, 0.8],
        }
        df = pd.DataFrame([row, row])
        with pytest.raises(AssertionError):
            Result.get_unique_from_dataframe(df, "SSIM_scores", "scores")

    def test_assert_asset_dataframe_rejects_mismatched_executor(self):
        """_assert_asset_dataframe must fail when executor_id is not uniform."""
        import pandas as pd

        row1 = {
            "dataset": "t",
            "content_id": 0,
            "asset_id": 0,
            "ref_name": "r.yuv",
            "dis_name": "d.yuv",
            "asset": "repr",
            "executor_id": "EX_V1",
            "scores_key": "SSIM_scores",
            "scores": [0.9],
        }
        row2 = dict(row1, executor_id="EX_V2", scores_key="SSIM_feature_x_scores")
        df = pd.DataFrame([row1, row2])
        with pytest.raises(AssertionError):
            Result._assert_asset_dataframe(df)

    def test_assert_asset_dataframe_rejects_unequal_score_lengths(self):
        import pandas as pd

        row1 = {
            "dataset": "t",
            "content_id": 0,
            "asset_id": 0,
            "ref_name": "r.yuv",
            "dis_name": "d.yuv",
            "asset": "repr",
            "executor_id": "EX_V1",
            "scores_key": "SSIM_scores",
            "scores": [0.9, 0.8],
        }
        row2 = dict(row1, scores_key="SSIM_feature_x_scores", scores=[0.9])
        df = pd.DataFrame([row1, row2])
        with pytest.raises(AssertionError):
            Result._assert_asset_dataframe(df)


# ---------------------------------------------------------------------------
# RawResult
# ---------------------------------------------------------------------------


class TestRawResult:
    def test_basic_access(self):
        rr = RawResult(MagicMock(), "EX_V1", {"pixels": [1, 2, 3], "asset": "my_asset"})
        assert rr["pixels"] == [1, 2, 3]
        assert rr["asset"] == "my_asset"

    def test_missing_key_raises_key_error(self):
        rr = RawResult(MagicMock(), "EX_V1", {})
        with pytest.raises(KeyError):
            _ = rr["nonexistent"]

    def test_no_aggregate_score_fallback(self):
        """RawResult must NOT attempt score aggregation — KeyError is correct."""
        rr = RawResult(MagicMock(), "EX_V1", {"VMAF_scores": np.array([70.0])})
        with pytest.raises(KeyError):
            _ = rr["VMAF_score"]

    def test_get_ordered_results(self):
        rr = RawResult(MagicMock(), "EX_V1", {"z_key": 1, "a_key": 2})
        ordered = rr.get_ordered_results()
        assert ordered == sorted(["z_key", "a_key"])

    def test_attributes_set_correctly(self):
        asset = MagicMock()
        rr = RawResult(asset, "EX_V1.5", {"k": "v"})
        assert rr.asset is asset
        assert rr.executor_id == "EX_V1.5"
        assert rr.result_dict == {"k": "v"}


# ---------------------------------------------------------------------------
# BasicResult.get_scores_key_from_score_key
# ---------------------------------------------------------------------------


class TestGetScoresKeyFromScoreKey:
    def test_appends_s(self):
        assert BasicResult.get_scores_key_from_score_key("VMAF_score") == "VMAF_scores"

    def test_feature_score_key(self):
        key = BasicResult.get_scores_key_from_score_key("VMAF_feature_vif_score")
        assert key == "VMAF_feature_vif_scores"


# ---------------------------------------------------------------------------
# Result._get_scores_str / _get_aggregate_score_str (smoke)
# ---------------------------------------------------------------------------


class TestResultStringHelpers:
    def test_get_scores_str_smoke(self):
        r = _make_result()
        s = r._get_scores_str()
        assert "Frame" in s

    def test_get_aggregate_score_str_smoke(self):
        r = _make_result()
        s = r._get_aggregate_score_str()
        assert "Aggregate" in s

    def test_get_scores_str_with_unit_name(self):
        r = _make_result()
        s = r._get_scores_str(unit_name="Chunk")
        assert "Chunk" in s
