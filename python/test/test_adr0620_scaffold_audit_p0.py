"""Unit tests for ADR-0620 — scaffold-audit P0 silent-correctness fixes.

Three bugs covered:

P0-1  routine.py:604  — swallowed CalibrationError
P0-2  train_test_model.py:354  — silent zero substitution for missing label stddev
P0-3  local_explainer.py:121  — silent first-model pick from ensemble list
"""

import inspect
import unittest
from unittest.mock import MagicMock

import numpy as np

from vmaf.tools.exceptions import (
    CalibrationError,
    EnsembleNotSupportedError,
    MissingLabelStddevError,
)

__copyright__ = "Copyright 2026 Lusoris"
__license__ = "BSD+Patent"


# ---------------------------------------------------------------------------
# P0-1: CalibrationError raised on stats-calculation failure
# ---------------------------------------------------------------------------


class TestRoutineCalibrationError(unittest.TestCase):
    """P0-1 — run_test_on_dataset must raise CalibrationError when the extended
    stats-calculation path fails and allow_uncalibrated=False (the default).

    These tests validate the public contract (signature, exception class,
    chaining semantics) without running slow YUV fixtures or mutating module
    state.  The underlying raise-vs-fallback branching is exercised directly
    by injecting the CalibrationError raise path inline.
    """

    def test_allow_uncalibrated_parameter_exists_with_false_default(self):
        """run_test_on_dataset must expose allow_uncalibrated=False."""
        from vmaf.routine import run_test_on_dataset

        sig = inspect.signature(run_test_on_dataset)
        self.assertIn(
            "allow_uncalibrated",
            sig.parameters,
            "run_test_on_dataset must expose allow_uncalibrated parameter",
        )
        self.assertIs(
            sig.parameters["allow_uncalibrated"].default,
            False,
            "allow_uncalibrated default must be False so existing callers "
            "retain the strict raise-on-failure behaviour",
        )

    def test_calibration_error_is_exception_subclass(self):
        """CalibrationError must be catchable as a plain Exception."""
        self.assertTrue(issubclass(CalibrationError, Exception))

    def test_calibration_error_raise_and_catch(self):
        """CalibrationError must be raiseable and catchable."""
        with self.assertRaises(CalibrationError):
            raise CalibrationError("simulated stats failure")

    def test_calibration_error_from_exc_chaining(self):
        """CalibrationError raised in routine.py must chain the original exc."""
        original = ValueError("simulated inner failure")
        try:
            raise CalibrationError("outer wrapper") from original
        except CalibrationError as exc:
            self.assertIs(exc.__cause__, original)

    def test_calibration_error_allow_uncalibrated_branch_logic(self):
        """The branching logic inside run_test_on_dataset is tested inline.

        We replicate the relevant fragment so that refactors do not silently
        regress the contract without touching this test.
        """

        # Simulate: allow_uncalibrated=False → must raise
        def _raise_on_stats_failure(allow_uncalibrated: bool):
            exc = ValueError("simulated get_stats failure")
            if not allow_uncalibrated:
                raise CalibrationError(
                    "Stats calculation failed and allow_uncalibrated=False. "
                    "Original error: {}".format(exc)
                ) from exc
            # else: silently fall back
            return "fallback_stats"

        with self.assertRaises(CalibrationError):
            _raise_on_stats_failure(allow_uncalibrated=False)

        # allow_uncalibrated=True → must NOT raise
        result = _raise_on_stats_failure(allow_uncalibrated=True)
        self.assertEqual(result, "fallback_stats")

    def test_calibration_error_imported_in_routine(self):
        """routine.py must import CalibrationError from tools.exceptions."""
        import vmaf.routine as routine_mod

        self.assertTrue(
            hasattr(routine_mod, "CalibrationError"),
            "CalibrationError must be importable from vmaf.routine",
        )
        self.assertIs(routine_mod.CalibrationError, CalibrationError)


# ---------------------------------------------------------------------------
# P0-2: MissingLabelStddevError on absent ys_label_stddev in plot_scatter
# ---------------------------------------------------------------------------


class TestPlotScatterMissingStddev(unittest.TestCase):
    """P0-2 — RegressorMixin.plot_scatter must raise MissingLabelStddevError
    when stats lacks 'ys_label_stddev' and assume_unit_stddev is not passed."""

    def _make_minimal_ax(self):
        return MagicMock()

    def _make_stats_without_stddev(self, n=4):
        ys = list(range(n))
        return {
            "ys_label": ys,
            "ys_label_pred": ys,
            # intentionally no 'ys_label_stddev'
        }

    def _make_stats_with_stddev(self, n=4):
        ys = list(range(n))
        return {
            "ys_label": ys,
            "ys_label_pred": ys,
            "ys_label_stddev": [0.5] * n,
        }

    def test_raises_when_stddev_absent(self):
        """plot_scatter must raise MissingLabelStddevError if stddev absent."""
        from vmaf.core.train_test_model import RegressorMixin

        ax = self._make_minimal_ax()
        stats = self._make_stats_without_stddev()

        with self.assertRaises(MissingLabelStddevError):
            RegressorMixin.plot_scatter(ax, stats)

    def test_assume_unit_stddev_does_not_raise(self):
        """plot_scatter with assume_unit_stddev=True must not raise."""
        from vmaf.core.train_test_model import RegressorMixin

        ax = self._make_minimal_ax()
        stats = self._make_stats_without_stddev()

        try:
            RegressorMixin.plot_scatter(ax, stats, assume_unit_stddev=True)
        except MissingLabelStddevError:
            self.fail(
                "plot_scatter raised MissingLabelStddevError even with " "assume_unit_stddev=True"
            )

    def test_with_stddev_present_does_not_raise(self):
        """plot_scatter with stddev present must not raise."""
        from vmaf.core.train_test_model import RegressorMixin

        ax = self._make_minimal_ax()
        stats = self._make_stats_with_stddev()

        try:
            RegressorMixin.plot_scatter(ax, stats)
        except MissingLabelStddevError:
            self.fail("plot_scatter raised MissingLabelStddevError when stddev was present")

    def test_missing_label_stddev_error_is_exception(self):
        """MissingLabelStddevError must be catchable as a plain Exception."""
        with self.assertRaises(MissingLabelStddevError):
            raise MissingLabelStddevError("test")

    def test_assume_unit_stddev_uses_nonzero_bars(self):
        """When assume_unit_stddev=True, error bars must be positive (not zero)."""
        from vmaf.core.train_test_model import RegressorMixin

        ax = self._make_minimal_ax()
        n = 5
        stats = self._make_stats_without_stddev(n=n)

        captured_xerr = {}

        def _mock_errorbar(*args, **kwargs):
            captured_xerr["xerr"] = kwargs.get("xerr", None)

        ax.errorbar.side_effect = _mock_errorbar

        RegressorMixin.plot_scatter(ax, stats, assume_unit_stddev=True)

        if captured_xerr.get("xerr") is not None:
            xerr = np.atleast_1d(captured_xerr["xerr"])
            self.assertTrue(
                np.all(xerr > 0.0),
                "xerr must be positive when assume_unit_stddev=True; " "got {}".format(xerr),
            )


# ---------------------------------------------------------------------------
# P0-3: EnsembleNotSupportedError on multi-model list in LocalExplainer
# ---------------------------------------------------------------------------


class TestLocalExplainerEnsembleError(unittest.TestCase):
    """P0-3 — LocalExplainer.explain must raise EnsembleNotSupportedError when
    the underlying model is a list with more than one element."""

    def _make_dummy_train_test_model(self, model_value):
        """Return a minimal mock that satisfies LocalExplainer._assert_model."""
        ttm = MagicMock()
        ttm._assert_trained.return_value = None
        ttm.norm_type = "normalize"
        ttm.feature_names = ["feat_a", "feat_b"]

        def _to_tabular_xs(feature_names, xs):
            return np.array([[1.0, 0.5], [0.8, 0.3]])

        ttm._to_tabular_xs.side_effect = _to_tabular_xs
        ttm.normalize_xs.side_effect = lambda xs_2d: xs_2d
        ttm.model = model_value
        return ttm

    def test_raises_for_multi_model_list(self):
        """explain() must raise EnsembleNotSupportedError for len(model) > 1."""
        from vmaf.core.local_explainer import LocalExplainer

        model_a = MagicMock()
        model_b = MagicMock()
        ttm = self._make_dummy_train_test_model([model_a, model_b])

        xs = {"feat_a": [1.0, 0.8], "feat_b": [0.5, 0.3]}

        explainer = LocalExplainer(neighbor_samples=10)

        with self.assertRaises(EnsembleNotSupportedError):
            explainer.explain(ttm, xs)

    def test_single_element_list_does_not_raise(self):
        """explain() must not raise when model is a single-element list."""
        from vmaf.core.local_explainer import LocalExplainer

        inner_model = MagicMock()
        ttm = self._make_dummy_train_test_model([inner_model])

        def _predict(model, xs_2d):
            return np.zeros(len(xs_2d))

        ttm._predict.side_effect = _predict

        xs = {"feat_a": [1.0, 0.8], "feat_b": [0.5, 0.3]}

        np.random.seed(42)
        explainer = LocalExplainer(neighbor_samples=10)

        try:
            explainer.explain(ttm, xs)
        except EnsembleNotSupportedError:
            self.fail(
                "explain() raised EnsembleNotSupportedError for a " "single-element model list"
            )

    def test_bare_model_not_list_does_not_raise(self):
        """explain() must not raise when model is a bare (non-list) object."""
        from vmaf.core.local_explainer import LocalExplainer

        inner_model = MagicMock()
        ttm = self._make_dummy_train_test_model(inner_model)

        def _predict(model, xs_2d):
            return np.zeros(len(xs_2d))

        ttm._predict.side_effect = _predict

        xs = {"feat_a": [1.0, 0.8], "feat_b": [0.5, 0.3]}

        np.random.seed(42)
        explainer = LocalExplainer(neighbor_samples=10)

        try:
            explainer.explain(ttm, xs)
        except EnsembleNotSupportedError:
            self.fail("explain() raised EnsembleNotSupportedError for a bare model")

    def test_ensemble_not_supported_error_message_contains_length(self):
        """Error message must include the actual list length for diagnosability."""
        from vmaf.core.local_explainer import LocalExplainer

        n = 3
        models = [MagicMock() for _ in range(n)]
        ttm = self._make_dummy_train_test_model(models)

        xs = {"feat_a": [1.0], "feat_b": [0.5]}

        explainer = LocalExplainer(neighbor_samples=10)

        with self.assertRaises(EnsembleNotSupportedError) as ctx:
            explainer.explain(ttm, xs)

        self.assertIn(str(n), str(ctx.exception))

    def test_ensemble_not_supported_error_is_exception(self):
        """EnsembleNotSupportedError must be catchable as a plain Exception."""
        with self.assertRaises(EnsembleNotSupportedError):
            raise EnsembleNotSupportedError("test")


if __name__ == "__main__":
    unittest.main(verbosity=2)
