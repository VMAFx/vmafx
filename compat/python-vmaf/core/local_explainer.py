import numpy as np
import sklearn.metrics
from sklearn.linear_model import Ridge

from vmaf import plt
from vmaf.tools.exceptions import EnsembleNotSupportedError
from vmaf.tools.misc import get_file_name_without_extension
from vmaf.tools.reader import YuvReader

# Copyright (c) 2016, Marco Tulio Correia Ribeiro
# All rights reserved.
#
# Redistribution and use in source and binary forms, with or without
# modification, are permitted provided that the following conditions are met:
#
# * Redistributions of source code must retain the above copyright notice, this
#   list of conditions and the following disclaimer.
#
# * Redistributions in binary form must reproduce the above copyright notice,
#   this list of conditions and the following disclaimer in the documentation
#   and/or other materials provided with the distribution.
#
# THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
# AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
# IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
# DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
# FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
# DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
# SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
# CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
# OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
# OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


class LocalExplainer(object):
    """Explains a TrainTestModel on a local data point.
    Adapted from:
    Lime: Explaining the predictions of any machine learning classifier
    https://github.com/marcotcr/lime"""

    def __init__(
        self,
        neighbor_std=1.0,
        neighbor_samples=5000,
        distance_metric="euclidean",
        kernel_width=3,
        model_regressor=None,
    ):
        """Init function.

        :param neighbor_std: standard deviation of neighborhood sampled
        :param neighbor_samples: number of samples of neighborhood
        :param distance_metric: distance metric used
        :param kernel_width: width for kernel function
        :param model_regressor: regressor to train local linear model. If None, use Ridge
        """
        self.neighbor_std = neighbor_std
        self.neighbor_samples = neighbor_samples
        self.distance_metric = distance_metric
        self.kernel_fn = lambda d: np.sqrt(np.exp(-(d**2) / kernel_width**2))
        self.model_regressor = (
            Ridge(alpha=1, fit_intercept=True) if model_regressor is None else model_regressor
        )

    def _assert_model(self, train_test_model):

        train_test_model._assert_trained()

        assert hasattr(
            train_test_model, "_to_tabular_xs"
        ), "train_test_model must have a method _to_tabular_xs()."

        assert train_test_model.norm_type != "none", (
            "If train_test_model has not been normalized, "
            "the sampled neighborhood may not be of the right shape."
        )

    @staticmethod
    def _single_model(train_test_model):
        model = train_test_model.model
        if not isinstance(model, list):
            return model
        if len(model) != 1:
            raise EnsembleNotSupportedError(
                "LocalExplainer received a model list of length {}. "
                "Explanation for multi-model ensembles is not yet "
                "defined. Either reduce the list to a single model or "
                "implement an ensemble aggregation strategy.".format(len(model))
            )
        return model[0]

    def _explain_sample(self, train_test_model, model, x_row, n_feature):
        neighbors = np.random.randn(self.neighbor_samples, n_feature) * self.neighbor_std
        neighbors += np.tile(x_row, (self.neighbor_samples, 1))
        neighbors = np.vstack([x_row, neighbors])
        distances = sklearn.metrics.pairwise_distances(
            neighbors, neighbors[0].reshape(1, -1), metric=self.distance_metric
        ).ravel()
        predictions = train_test_model._predict(model, neighbors)
        self.model_regressor.fit(
            neighbors,
            predictions,
            sample_weight=self.kernel_fn(distances),
        )
        return self.model_regressor.coef_.copy()

    def explain(self, train_test_model, xs):
        """Explain data points.

        :param train_test_model: a trained TrainTestModel object
        :param xs: same xs as in TrainTestModel.predict(xs)
        :return: exps: explanations, where exps['feature_weights'] has the
        feature weights (in num_sample x num_feature 2D array)
        """

        self._assert_model(train_test_model)

        feature_names = train_test_model.feature_names
        for name in feature_names:
            assert name in xs

        xs_2d = train_test_model._to_tabular_xs(feature_names, xs)

        xs_2d_unnormalized = xs_2d.copy()

        # normalize xs
        xs_2d = train_test_model.normalize_xs(xs_2d)

        # for each row of xs, repeat feature of a unit (e.g. frame),
        # generate a new 2d_array by sampling its neighborhood
        n_sample, n_feature = xs_2d.shape
        feature_weights = np.zeros([n_sample, n_feature])
        model = self._single_model(train_test_model)
        for i_sample in range(n_sample):
            feature_weights[i_sample, :] = self._explain_sample(
                train_test_model,
                model,
                xs_2d[i_sample, :],
                n_feature,
            )

        return {
            "feature_weights": feature_weights,
            "features": xs_2d_unnormalized,
            "features_normalized": xs_2d,
            "feature_names": feature_names,
        }

    @staticmethod
    def assert_explanations(exps, assets=None, ys=None, ys_pred=None):
        N = exps["feature_weights"].shape[0]
        assert exps["features_normalized"].shape[0] == N
        if assets is not None:
            assert len(assets) == N
        if ys is not None:
            assert len(ys["label"]) == N
        if ys_pred is not None:
            assert len(ys_pred) == N
        return N

    @classmethod
    def print_explanations(cls, exps, assets=None, ys=None, ys_pred=None):

        # asserts
        N = cls.assert_explanations(exps, assets, ys, ys_pred)

        print("Features: {}".format(exps["feature_names"]))

        for n in range(N):
            weights = exps["feature_weights"][n]
            features = exps["features_normalized"][n]

            asset = assets[n] if assets is not None else None
            y = ys["label"][n] if ys is not None else None
            y_pred = ys_pred[n] if ys_pred is not None else None

            print(
                "{ref}".format(
                    ref=(
                        get_file_name_without_extension(asset.ref_path)
                        if asset is not None
                        else "Asset {}".format(n)
                    )
                )
            )
            if asset is not None:
                print(
                    "\tDistorted: {dis}".format(dis=get_file_name_without_extension(asset.dis_path))
                )
            if y is not None:
                print("\tground truth: {y:.3f}".format(y=y))
            if y_pred is not None:
                print("\tpredicted: {y_pred:.3f}".format(y_pred=y_pred))
            print("\tfeature value: {}".format(features))
            print("\tfeature weight: {}".format(weights))

    @classmethod
    def _load_asset_image(cls, asset):
        if asset is None:
            return None
        width, height = asset.dis_width_height
        with YuvReader(
            filepath=asset.dis_path,
            width=width,
            height=height,
            yuv_type=asset.dis_yuv_type,
        ) as yuv_reader:
            return next(iter(yuv_reader))[0].astype(np.double)

    @classmethod
    def _plot_one_explanation(cls, exps, index, asset, y, y_pred):
        weights = exps["feature_weights"][index]
        features = exps["features"][index]
        normalized = exps["features_normalized"][index]
        image = cls._load_asset_image(asset)
        title_parts = []
        if asset is not None:
            title_parts.append(get_file_name_without_extension(asset.ref_path))
        if y is not None:
            title_parts.append("ground truth: {:.3f}".format(y))
        if y_pred is not None:
            title_parts.append("predicted: {:.3f}".format(y_pred))

        assert len(weights) == len(features)
        positions = np.arange(len(weights)) + 0.1
        fig = plt.figure()
        ax_top = plt.subplot(2, 1, 1)
        ax_left = plt.subplot(2, 3, 4)
        ax_mid = plt.subplot(2, 3, 5, sharey=ax_left)
        ax_right = plt.subplot(2, 3, 6, sharey=ax_left)
        if image is not None:
            ax_top.imshow(image, cmap="Greys_r")
        ax_top.get_xaxis().set_visible(False)
        ax_top.get_yaxis().set_visible(False)
        ax_top.set_title("\n".join(title_parts))
        ax_left.barh(positions, features, color="b", label="feature")
        ax_left.set_xticks(np.arange(0, 1.1, 0.2))
        ax_left.set_yticks(positions + 0.35)
        ax_left.set_yticklabels(exps["feature_names"])
        ax_left.set_title("feature")
        ax_mid.barh(positions, normalized, color="g", label="fnormal")
        ax_mid.get_yaxis().set_visible(False)
        ax_mid.set_title("fnormal")
        ax_right.barh(positions, weights, color="r", label="weight")
        ax_right.get_yaxis().set_visible(False)
        ax_right.set_title("weight")
        plt.tight_layout()
        return fig

    @classmethod
    def plot_explanations(cls, exps, assets=None, ys=None, ys_pred=None):

        # asserts
        N = cls.assert_explanations(exps, assets, ys, ys_pred)

        figs = []
        for n in range(N):
            asset = assets[n] if assets is not None else None
            y = ys["label"][n] if ys is not None else None
            y_pred = ys_pred[n] if ys_pred is not None else None
            figs.append(cls._plot_one_explanation(exps, n, asset, y, y_pred))

        return figs

    @classmethod
    def select_from_exps(cls, exps, indexs):
        # asserts
        N = cls.assert_explanations(exps)
        for index in indexs:
            assert index < N
        return {
            "feature_weights": exps["feature_weights"][indexs, :],
            "features": exps["features"][indexs, :],
            "features_normalized": exps["features_normalized"][indexs, :],
            "feature_names": exps["feature_names"],
        }
