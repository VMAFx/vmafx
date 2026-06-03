# Copyright 2026 Lusoris
# SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
"""
T7-32 regression: verify that ``cv_on_dataset`` and
``explain_model_on_dataset`` propagate ``feature_option_dict`` to
``FeatureAssembler`` consistently with ``VmafQualityRunner``.

Before T7-32 both call sites hard-coded ``feature_option_dict=None``
(carrying a ``FIXME`` comment about inconsistent behaviour with
``VmafQualityRunner``).  After the fix:

* ``cv_on_dataset`` looks up ``feature_param.feature_optional_dict``
  when the param object exposes it, falling back to ``None`` when it
  does not.
* ``explain_model_on_dataset`` reads ``model.model_dict["feature_opts_dicts"]``
  with a ``None`` fallback, mirroring ``VmafQualityRunner``'s contract.

The tests here patch ``FeatureAssembler`` so the suite stays fast
and YUV-fixture-free; we assert the keyword arguments passed to it
are correct under both ``None`` and populated configurations.
"""

import os
import sys
import types
import unittest
from unittest.mock import MagicMock, patch

import numpy as np


def _stub_external_modules():
    """Stub heavy / display-only imports so this test runs in CI even
    without a display / without ``sureal`` / without ``libsvm``.  T7-32
    is a pure wiring change in ``routine.py``; the regression test
    only inspects the kwargs handed to ``FeatureAssembler``, so the
    transitive heavyweight ML deps can be safely faked.
    """
    if "matplotlib" not in sys.modules:
        sys.modules["matplotlib"] = MagicMock()
        sys.modules["matplotlib.pyplot"] = MagicMock()
    # ``sureal`` and ``libsvm`` are real upstream test dependencies of
    # the vmaf python package (see python/requirements.txt).  No
    # stubbing — the test environment is expected to have them
    # installed, matching every other test in python/test/.


_stub_external_modules()


class _FakeAsset:
    """Minimal ``Asset`` stand-in for ``read_dataset``."""

    def __init__(self, content_id, groundtruth=1.0):
        self.content_id = content_id
        self.groundtruth = groundtruth
        self.groundtruth_std = 0.0
        self.raw_groundtruth = None
        # ``explain_model_on_dataset`` prints the dis path before
        # constructing the FeatureAssembler.
        self.dis_path = "/tmp/t7_32_fake_dis_{0}.yuv".format(content_id)


class _FakeResult:
    def set_score_aggregate_method(self, method):
        del method


class _FakeFAssembler:
    """Records constructor kwargs and yields one empty result per asset."""

    instances = []

    def __init__(self, **kwargs):
        self.kwargs = kwargs
        self.results = [_FakeResult() for _ in kwargs.get("assets", [])]
        type(self).instances.append(self)

    def run(self):
        return None


class CvOnDatasetFeatureOptionDictTest(unittest.TestCase):
    """``cv_on_dataset`` propagates ``feature_optional_dict`` (T7-32).

    Two sub-cases:
      1. ``feature_param`` lacks ``feature_optional_dict`` -> assembler
         receives ``feature_option_dict=None`` (legacy default).
      2. ``feature_param`` exposes ``feature_optional_dict`` -> assembler
         receives that exact dict.
    """

    def setUp(self):
        _FakeFAssembler.instances = []

    def _drive(self, feature_param):
        from vmaf import routine

        dataset = types.SimpleNamespace(dataset_name="t7-32-fake")
        model_param = types.SimpleNamespace(model_type="LIBSVMNUSVR", model_param_dict={})
        ax = MagicMock()
        result_store = MagicMock()
        contentid_groups = [[0]]

        fake_assets = [_FakeAsset(0)]

        with (
            patch.object(routine, "read_dataset", return_value=fake_assets),
            patch.object(routine, "construct_kfold_list", return_value=[([0], [0])]),
            patch.object(routine, "FeatureAssembler", _FakeFAssembler),
            patch.object(routine, "ModelCrossValidation") as mcv,
            patch.object(routine, "TrainTestModel") as ttm,
        ):
            mcv.run_kfold_cross_validation.return_value = (
                {"ys_label": np.array([1.0]), "ys_label_pred": np.array([1.0])},
                None,
            )
            ttm.find_subclass.return_value = MagicMock()
            ttm.format_stats_for_print = staticmethod(lambda s: "")
            ttm.format_stats_for_plot = staticmethod(lambda s: "")
            ttm.plot_scatter = staticmethod(lambda *a, **k: None)
            try:
                routine.cv_on_dataset(
                    dataset,
                    feature_param,
                    model_param,
                    ax,
                    result_store,
                    contentid_groups,
                )
            except Exception:
                # We only care that FeatureAssembler was constructed
                # with the right wiring; downstream stat plotting
                # exercises a deeper code path the mock cannot fully
                # satisfy.  The first FeatureAssembler instance is
                # captured before any of that.
                pass

        return _FakeFAssembler.instances

    def test_cv_on_dataset_passes_none_when_param_lacks_optional_dict(self):
        feature_param = types.SimpleNamespace(feature_dict={"VMAF_feature": ["vif"]})
        instances = self._drive(feature_param)
        self.assertGreaterEqual(len(instances), 1)
        self.assertIsNone(instances[0].kwargs["feature_option_dict"])

    def test_cv_on_dataset_passes_populated_optional_dict(self):
        opt = {"VMAF_feature": {"adm_ref_display_height": 540}}
        feature_param = types.SimpleNamespace(
            feature_dict={"VMAF_feature": ["vif"]},
            feature_optional_dict=opt,
        )
        instances = self._drive(feature_param)
        self.assertGreaterEqual(len(instances), 1)
        self.assertEqual(instances[0].kwargs["feature_option_dict"], opt)


class ExplainModelOnDatasetFeatureOptionDictTest(unittest.TestCase):
    """``explain_model_on_dataset`` reads ``feature_opts_dicts`` from
    the model dict (T7-32).

    Mirrors ``VmafQualityRunner`` which also draws this key from the
    serialised model.
    """

    def setUp(self):
        _FakeFAssembler.instances = []

    def _drive(self, model_dict):
        from vmaf import routine

        model = MagicMock()
        model.model_dict = model_dict
        model.get_xs_from_results.return_value = {}
        model.get_ys_from_results.return_value = {"label": np.array([1.0])}
        model.predict.return_value = {"ys_label_pred": np.array([1.0])}

        explainer = MagicMock()
        explainer.explain.return_value = []

        fake_assets = [_FakeAsset(0)]

        with (
            patch.object(routine, "import_python_file"),
            patch.object(routine, "read_dataset", return_value=fake_assets),
            patch.object(routine, "FeatureAssembler", _FakeFAssembler),
            patch.object(routine, "FileSystemResultStore"),
            patch.object(routine, "LocalExplainer", return_value=explainer),
            patch.object(routine, "DisplayConfig"),
        ):
            try:
                routine.explain_model_on_dataset(
                    model,
                    test_assets_selected_indexs=[0],
                    test_dataset_filepath="/dev/null",
                )
            except Exception:
                pass

        return _FakeFAssembler.instances

    def test_explain_passes_none_when_model_lacks_feature_opts_dicts(self):
        model_dict = {"feature_dict": {"VMAF_feature": ["vif"]}}
        instances = self._drive(model_dict)
        self.assertGreaterEqual(len(instances), 1)
        self.assertIsNone(instances[0].kwargs["feature_option_dict"])

    def test_explain_passes_populated_feature_opts_dicts(self):
        opt = {"VMAF_feature": {"adm_ref_display_height": 540}}
        model_dict = {
            "feature_dict": {"VMAF_feature": ["vif"]},
            "feature_opts_dicts": opt,
        }
        instances = self._drive(model_dict)
        self.assertGreaterEqual(len(instances), 1)
        self.assertEqual(instances[0].kwargs["feature_option_dict"], opt)


if __name__ == "__main__":
    unittest.main(verbosity=2)
