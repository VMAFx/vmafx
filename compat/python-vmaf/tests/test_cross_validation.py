# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Clear
#
# Unit tests for compat/python-vmaf/core/cross_validation.py
#
# Covers ModelCrossValidation: k-fold splitter, integer-fold expansion,
# nested k-fold grid-search, _find_most_frequent_dict, _sample_model_param_list,
# and print_output / format_stats helpers. No model binary required.
"""Pytest cases for vmaf.core.cross_validation — ModelCrossValidation."""

from __future__ import annotations

import pytest

from vmaf.core.cross_validation import ModelCrossValidation

# ---------------------------------------------------------------------------
# Helpers: lightweight fake train/test model class
# ---------------------------------------------------------------------------


def _make_model_class(srcc: float = 0.9, pcc: float = 0.85, rmse: float = 5.0):
    """Return a minimal fake train-test model class for cross-validation tests.

    The class satisfies the interface expected by ModelCrossValidation:
    - get_xys_from_results(results, indices)
    - get_xs_from_results(results, indices)
    - get_ys_from_results(results, indices)
    - __init__(model_param, logger, optional_dict2)
    - train(xys)
    - evaluate(xs, ys) -> stats dict
    - aggregate_stats_list(statss) -> aggregated stats
    - get_objective_score(stats, score_type) -> float
    """

    class FakeModel:
        def __init__(self, model_param, logger=None, optional_dict2=None):
            self.model_param = model_param

        def train(self, xys):
            pass

        def evaluate(self, xs, ys):
            return {"SRCC": srcc, "PCC": pcc, "RMSE": rmse}

    class FakeModelClass:
        """Mimics the class-level interface of TrainTestModel."""

        reset_called = 0

        @classmethod
        def reset(cls):
            cls.reset_called += 1

        @classmethod
        def get_xys_from_results(cls, results, indexs=None):
            # Return a minimal xys dict; content doesn't matter for CV logic tests.
            n = len(results) if indexs is None else len(indexs)
            return {"label": [float(i) for i in range(n)], "content_id": list(range(n))}

        @classmethod
        def get_xs_from_results(cls, results, indexs=None):
            n = len(results) if indexs is None else len(indexs)
            return {"feature_x": [float(i) for i in range(n)]}

        @classmethod
        def get_ys_from_results(cls, results, indexs=None):
            n = len(results) if indexs is None else len(indexs)
            return {"label": [float(i) for i in range(n)], "content_id": list(range(n))}

        def __init__(self, model_param, logger=None, optional_dict2=None):
            self._model = FakeModel(model_param, logger, optional_dict2)

        def train(self, xys):
            self._model.train(xys)

        def evaluate(self, xs, ys):
            return self._model.evaluate(xs, ys)

        @classmethod
        def aggregate_stats_list(cls, statss):
            # Simple element-wise mean of SRCC, PCC, RMSE.
            keys = ("SRCC", "PCC", "RMSE")
            return {k: sum(s[k] for s in statss) / len(statss) for k in keys}

        @staticmethod
        def get_objective_score(stats, score_type="SRCC"):
            return stats.get(score_type, 0.0)

    return FakeModelClass


# ---------------------------------------------------------------------------
# run_cross_validation
# ---------------------------------------------------------------------------


class TestRunCrossValidation:
    def test_basic_run_returns_output_keys(self):
        mc = _make_model_class()
        results = list(range(10))  # proxy for BasicResult list
        train_idx = list(range(8))
        test_idx = list(range(8, 10))
        out = ModelCrossValidation.run_cross_validation(mc, {}, results, train_idx, test_idx)
        assert "stats" in out
        assert "model" in out
        assert "contentids" in out

    def test_stats_have_required_keys(self):
        mc = _make_model_class(srcc=0.8, pcc=0.75, rmse=3.0)
        results = list(range(10))
        out = ModelCrossValidation.run_cross_validation(
            mc, {}, results, list(range(8)), list(range(8, 10))
        )
        stats = out["stats"]
        assert "SRCC" in stats
        assert "PCC" in stats
        assert "RMSE" in stats

    def test_srcc_value_matches_fake_model(self):
        mc = _make_model_class(srcc=0.77)
        results = list(range(10))
        out = ModelCrossValidation.run_cross_validation(
            mc, {}, results, list(range(8)), list(range(8, 10))
        )
        assert out["stats"]["SRCC"] == pytest.approx(0.77)


# ---------------------------------------------------------------------------
# run_kfold_cross_validation — integer kfold
# ---------------------------------------------------------------------------


class TestRunKfoldCrossValidationInteger:
    def test_kfold_integer_produces_output(self):
        mc = _make_model_class()
        results = list(range(20))
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=4)
        assert "aggr_stats" in out
        assert "statss" in out
        assert "models" in out

    def test_kfold_statss_length_equals_k(self):
        mc = _make_model_class()
        results = list(range(20))
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=4)
        assert len(out["statss"]) == 4

    def test_kfold_models_length_equals_k(self):
        mc = _make_model_class()
        results = list(range(20))
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=4)
        assert len(out["models"]) == 4

    def test_kfold_contentids_populated(self):
        mc = _make_model_class()
        results = list(range(20))
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=4)
        assert out["contentids"] is not None
        assert len(out["contentids"]) > 0

    def test_kfold_2_folds_works(self):
        mc = _make_model_class()
        results = list(range(10))
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=2)
        assert len(out["statss"]) == 2

    def test_kfold_reset_called_if_available(self):
        mc = _make_model_class()
        mc.reset_called = 0
        results = list(range(8))
        ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=2)
        assert mc.reset_called == 2  # once per fold


# ---------------------------------------------------------------------------
# run_kfold_cross_validation — list kfold (LOSO-style)
# ---------------------------------------------------------------------------


class TestRunKfoldCrossValidationList:
    def test_list_kfold_produces_output(self):
        mc = _make_model_class()
        results = list(range(12))
        # 3 folds of 4 items each — LOSO-style
        kfold = [list(range(0, 4)), list(range(4, 8)), list(range(8, 12))]
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=kfold)
        assert "aggr_stats" in out

    def test_list_kfold_statss_length_equals_folds(self):
        mc = _make_model_class()
        results = list(range(12))
        kfold = [list(range(0, 4)), list(range(4, 8)), list(range(8, 12))]
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=kfold)
        assert len(out["statss"]) == 3

    def test_single_item_folds_each_fold_has_one_test(self):
        """Each fold is a single item — leave-one-out style."""
        mc = _make_model_class()
        results = list(range(5))
        kfold = [[i] for i in range(5)]
        out = ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=kfold)
        assert len(out["statss"]) == 5

    def test_kfold_too_short_raises(self):
        mc = _make_model_class()
        results = list(range(4))
        with pytest.raises(AssertionError):
            ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold=[[0, 1]])

    def test_invalid_kfold_type_raises(self):
        mc = _make_model_class()
        results = list(range(4))
        with pytest.raises(AssertionError):
            ModelCrossValidation.run_kfold_cross_validation(mc, {}, results, kfold="invalid")


# ---------------------------------------------------------------------------
# _find_most_frequent_dict
# ---------------------------------------------------------------------------


class TestFindMostFrequentDict:
    _fn = staticmethod(ModelCrossValidation._find_most_frequent_dict)

    def test_single_dict_returns_it(self):
        d = {"a": 1, "b": 2}
        result, count = self._fn([d])
        assert result == d
        assert count == 1

    def test_two_identical_dicts_count_two(self):
        d = {"x": 3}
        result, count = self._fn([d, d])
        assert result == d
        assert count == 2

    def test_majority_wins(self):
        d1 = {"x": 1}
        d2 = {"x": 2}
        result, count = self._fn([d1, d2, d1, d1])
        assert result == d1
        assert count == 3

    def test_multiple_keys_dict(self):
        d1 = {"a": 1, "b": 2}
        d2 = {"a": 3, "b": 4}
        result, count = self._fn([d1, d2, d1])
        assert result == d1
        assert count == 2


# ---------------------------------------------------------------------------
# _assert_grid_search / _assert_random_search
# ---------------------------------------------------------------------------


class TestAssertSearchType:
    def test_grid_search_valid(self):
        sr = {"C": [1, 10, 100], "gamma": [0.001, 0.01]}
        ModelCrossValidation._assert_grid_search(sr)  # must not raise

    def test_grid_search_non_list_value_raises(self):
        sr = {"C": "not_a_list"}
        with pytest.raises(AssertionError):
            ModelCrossValidation._assert_grid_search(sr)

    def test_random_search_list_value_valid(self):
        sr = {"C": [1, 10, 100]}
        ModelCrossValidation._assert_random_search(sr)  # must not raise

    def test_random_search_range_dict_valid(self):
        sr = {"C": {"low": 0.1, "high": 100.0, "decimal": 2}}
        ModelCrossValidation._assert_random_search(sr)  # must not raise

    def test_random_search_invalid_range_raises(self):
        sr = {"C": {"low": 0.1}}  # missing 'high' and 'decimal'
        with pytest.raises(AssertionError):
            ModelCrossValidation._assert_random_search(sr)


# ---------------------------------------------------------------------------
# _sample_model_param_list
# ---------------------------------------------------------------------------


class TestSampleModelParamList:
    def test_returns_correct_count(self):
        sr = {"C": [1, 10], "kernel": ["rbf", "linear"]}
        samples = ModelCrossValidation._sample_model_param_list(sr, 50)
        assert len(samples) == 50

    def test_each_sample_is_dict_with_all_keys(self):
        sr = {"alpha": [0.1, 0.5, 1.0], "beta": [0, 1]}
        samples = ModelCrossValidation._sample_model_param_list(sr, 20)
        for s in samples:
            assert set(s.keys()) == {"alpha", "beta"}

    def test_values_within_range_dict(self):
        sr = {"C": {"low": 1.0, "high": 10.0, "decimal": 2}}
        samples = ModelCrossValidation._sample_model_param_list(sr, 100)
        for s in samples:
            assert 1.0 <= s["C"] <= 10.0

    def test_values_from_list_are_members(self):
        choices = [1, 5, 10, 50]
        sr = {"n_estimators": choices}
        samples = ModelCrossValidation._sample_model_param_list(sr, 50)
        for s in samples:
            assert s["n_estimators"] in choices


# ---------------------------------------------------------------------------
# format_stats / print_output (smoke tests — no assertions on stdout content)
# ---------------------------------------------------------------------------


class TestFormatStats:
    def test_format_stats_returns_string(self):
        stats = {"SRCC": 0.9, "PCC": 0.85, "RMSE": 3.2}
        s = ModelCrossValidation.format_stats(stats)
        assert isinstance(s, str)
        assert "SRCC" in s
        assert "PCC" in s
        # format_stats renders the RMSE field as "MSE" in the output string.
        assert "MSE" in s

    def test_format_stats_values_present(self):
        stats = {"SRCC": 0.75, "PCC": 0.80, "RMSE": 2.5}
        s = ModelCrossValidation.format_stats(stats)
        assert "0.750" in s
        assert "0.800" in s
        assert "2.500" in s


class TestPrintOutput:
    def test_print_output_with_stats_key(self, capsys):
        out = {"stats": {"SRCC": 0.9, "PCC": 0.85, "RMSE": 3.2}}
        ModelCrossValidation.print_output(out)
        captured = capsys.readouterr()
        assert "Stats" in captured.out

    def test_print_output_with_aggr_stats_key(self, capsys):
        out = {"aggr_stats": {"SRCC": 0.9, "PCC": 0.85, "RMSE": 3.2}}
        ModelCrossValidation.print_output(out)
        captured = capsys.readouterr()
        assert "Aggregated" in captured.out

    def test_print_output_with_statss_and_model_params(self, capsys):
        out = {
            "statss": [{"SRCC": 0.9, "PCC": 0.85, "RMSE": 3.2}],
            "model_params": [{"C": 1}],
        }
        ModelCrossValidation.print_output(out)
        captured = capsys.readouterr()
        assert "Fold 0" in captured.out

    def test_print_output_empty_dict_is_noop(self, capsys):
        ModelCrossValidation.print_output({})
        captured = capsys.readouterr()
        assert captured.out == ""
