from abc import ABCMeta, abstractmethod

import numpy as np
import scipy.interpolate
import scipy.special
import scipy.stats
from numpy.linalg import lstsq

from vmaf.core.mixin import TypeVersionEnabled
from vmaf.tools.decorator import override
from vmaf.tools.misc import empty_object, indices
from vmaf.tools.sigproc import calpvalue, fastDeLong, significanceBinomial

_COMPARISON_VALUE_2 = 2
_COMPARISON_VALUE_NEG_2 = -2

__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"


class PerfMetric(TypeVersionEnabled):

    __metaclass__ = ABCMeta

    @classmethod
    @abstractmethod
    def _preprocess(cls, groundtruths, predictions, **kwargs):
        raise NotImplementedError

    @classmethod
    @abstractmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        raise NotImplementedError

    def __init__(self, groundtruths, predictions):
        """
        Performance metric on quality metrics
        :param groundtruths: either list of real numbers (aggregate scores like
        MOS or DMOS or MLE), or list of lists of real numbers (list of raw scores)
        :param predictions: list of real numbers
        :return:
        """
        TypeVersionEnabled.__init__(self)
        self.groundtruths = groundtruths
        self.predictions = predictions
        self._assert_args()

    def _assert_args(self):
        assert len(self.groundtruths) == len(
            self.predictions
        ), "The lengths of groundtruth labels and predictions do not match."

    def evaluate(self, **kwargs):
        """
        :return: ret - a dictionary with 'score' and other keys
        """
        groundtruths, predictions = self._preprocess(self.groundtruths, self.predictions, **kwargs)
        result = self._evaluate(groundtruths, predictions, **kwargs)
        assert "score" in result, "Score does not exist in result."
        return result


class RawScorePerfMetric(PerfMetric):
    """
    Groundtruth is a list of raw scores (list of lists of real numbers)
    """

    @override(PerfMetric)
    def _assert_args(self):
        super(RawScorePerfMetric, self)._assert_args()

        # require the raw scores to be more than 1
        for groundtruth in self.groundtruths:
            assert hasattr(groundtruth, "__len__") and len(groundtruth) > 1


class AucPerfMetric(RawScorePerfMetric):
    """
    # % The method is described in the paper:
    # % L. Krasula, K. Fliegel, P. Le Callet, M.Klima, "On the accuracy of
    # % objective image and video quality models: New methodology for
    # % performance evaluation", QoMEX 2016.
    # % When you use our method in your research, please, cite the above stated
    # % paper.
    # %
    # % Copyright (c) 2016
    # % Lukas Krasula <l.krasula@gmail.com>
    #
    # % Permission to use, copy, modify, and/or distribute this software for any
    # % purpose with or without fee is hereby granted, provided that the above
    # % copyright notice and this permission notice appear in all copies.
    # %
    # % THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
    # % WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
    # % MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHORS BE LIABLE FOR
    # % ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
    # % WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
    # % ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
    # % OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
    # %
    # % This software also uses the code described in:
    # % X. Sun and W. Xu, "Fast Implementation of DeLong's Algorithm for
    # % Comparing the Areas Under Correlated Receiver Operating Characteristic
    # % Curves," IEEE Signal Processing Letters, vol. 21, no. 11, pp. 1389-1393,
    # % 2014.
    # %
    # % and
    # %
    # % P. Hanhart, L. Krasula, P. Le Callet, T. Ebrahimi, "How to benchmark
    # % objective quality metrics from pair comparison data", QoMEX 2016.
    """

    TYPE = "AUC"
    VERSION = "0.1"

    @classmethod
    @override(PerfMetric)
    def _preprocess(cls, groundtruths, predictions, **kwargs):
        return groundtruths, predictions

    @staticmethod
    def _pairwise_delong(auc, covariance):
        metric_count = len(auc)
        p_values = np.ones([metric_count, metric_count])
        for first in range(metric_count - 1):
            for second in range(first + 1, metric_count):
                selection = [first, second]
                p_values[first, second] = calpvalue(
                    auc[selection], covariance[np.ix_(selection, selection)]
                )
                p_values[second, first] = p_values[first, second]
        return p_values

    @classmethod
    def _different_similar_performance(cls, score_differences, significance):
        different = np.abs(score_differences[:, indices(significance[0], lambda value: value != 0)])
        similar = np.abs(score_differences[:, indices(significance[0], lambda value: value == 0)])
        samples = empty_object()
        samples.spsizes = [different.shape[1], similar.shape[1]]
        samples.ratings = np.hstack([different, similar])
        auc, covariance, _, _ = fastDeLong(samples)
        threshold = np.percentile(similar, 95, axis=1)
        return auc, cls._pairwise_delong(auc, covariance), threshold

    @classmethod
    def _better_worse_performance(cls, score_differences, significance):
        better_positive = score_differences[:, indices(significance[0], lambda value: value == 1)]
        better_negative = score_differences[:, indices(significance[0], lambda value: value == -1)]
        better = np.hstack([better_positive, -better_negative])
        worse = -better
        samples = empty_object()
        samples.ratings = np.hstack([better, worse])
        samples.spsizes = [better.shape[1], worse.shape[1]]
        auc, covariance, _, _ = fastDeLong(samples)
        sample_count = better.shape[1] + worse.shape[1]
        correct = np.array(
            [float(np.sum(row > 0) + np.sum(-row < 0)) / sample_count for row in better]
        )
        binomial = np.ones([len(correct), len(correct)])
        for first in range(len(correct) - 1):
            for second in range(first + 1, len(correct)):
                binomial[first, second] = significanceBinomial(
                    correct[first], correct[second], sample_count
                )
                binomial[second, first] = binomial[first, second]
        return auc, cls._pairwise_delong(auc, covariance), correct, binomial

    @classmethod
    def _metrics_performance(cls, objScoDif, signif):
        auc_ds, p_ds, threshold = cls._different_similar_performance(objScoDif, signif)
        auc_bw, p_bw, correct, p_correct = cls._better_worse_performance(objScoDif, signif)
        return {
            "AUC_DS": auc_ds,
            "pDS_DL": p_ds,
            "AUC_BW": auc_bw,
            "pBW_DL": p_bw,
            "CC_0": correct,
            "pCC0_b": p_correct,
            "THR": threshold,
        }

    @staticmethod
    def _significance(first_scores, second_scores):
        first_mean = np.mean(first_scores)
        second_mean = np.mean(second_scores)
        variance = np.var(first_scores, ddof=1) / len(first_scores)
        variance += np.var(second_scores, ddof=1) / len(second_scores)
        if variance == 0.0:
            variance = 1e-8
        z_score = (first_mean - second_mean) / np.sqrt(variance)
        if z_score < _COMPARISON_VALUE_NEG_2:
            return -1
        if z_score > _COMPARISON_VALUE_2:
            return 1
        return 0

    @classmethod
    def _significance_matrix(cls, groundtruths):
        sample_count = len(groundtruths)
        significance = np.zeros([sample_count, sample_count])
        for first, first_scores in enumerate(groundtruths):
            for second, second_scores in enumerate(groundtruths):
                significance[first, second] = cls._significance(first_scores, second_scores)
        return significance

    @staticmethod
    def _prediction_differences(predictions, sample_count):
        metric_predictions = predictions if isinstance(predictions[0], list) else [predictions]
        differences = np.zeros([len(metric_predictions), sample_count * sample_count])
        for metric_index, metric in enumerate(metric_predictions):
            matrix = np.subtract.outer(metric, metric)
            differences[metric_index, :] = matrix.reshape(1, sample_count * sample_count)
        return differences

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        if isinstance(groundtruths, (list, tuple)) and isinstance(groundtruths[0], dict):
            raise TypeError("{} cannot handle dictionary-style daataset yet.".format(cls.__name__))
        sample_count = len(groundtruths)
        significance = cls._significance_matrix(groundtruths)
        differences = cls._prediction_differences(predictions, sample_count)
        results = cls._metrics_performance(
            differences, significance.reshape(1, sample_count * sample_count)
        )
        results["score"] = results["AUC_BW"]
        if isinstance(predictions[0], list):
            return results
        return {key: value[0] for key, value in results.items()}

    def _assert_args(self):
        if isinstance(self.predictions[0], list):
            for metric in self.predictions:
                assert len(self.groundtruths) == len(
                    metric
                ), "The lengths of groundtruth labels and predictions do not match."
                for score in metric:
                    assert isinstance(
                        score, (float, int)
                    ), "Predictions need to be a list of lists of numbers."

        else:
            assert len(self.groundtruths) == len(
                self.predictions
            ), "The lengths of groundtruth labels and predictions do not match."


class ResolvingPowerPerfMetric(RawScorePerfMetric):
    """
    The method is described in the paper:
    M. H. Pinson, S. Wolf, "Techniques for Evaluating Objective Video Quality Models Using
    Overlapping Subjective Data Sets", NTIA Technical Report TR-09-457.
    """

    TYPE = "ResPow"
    VERSION = "0.1"

    @staticmethod
    def sigmoid_adjust_raw(xs, ys):
        ys_max = np.max(ys) + 0.1
        ys_min = np.min(ys) - 0.1

        ys_ = np.mean(np.array(ys), axis=1)

        # normalize to [0, 1]
        ys_ = (ys_ - ys_min) / (ys_max - ys_min)

        zs = -np.log(1.0 / ys_.T - 1.0)
        Y_mtx = np.array((np.ones(len(ys_)), zs)).T
        x_vec = np.array([xs]).T
        a_b = lstsq(Y_mtx, x_vec, rcond=-1)[0]
        a = a_b.item(0)
        b = a_b.item(1)

        xs = 1.0 / (1.0 + np.exp(-(np.array(xs) - a) / b))

        # denormalize
        return xs * (ys_max - ys_min) + ys_min

    @classmethod
    @override(PerfMetric)
    def _preprocess(cls, groundtruths, predictions, **kwargs):
        enable_mapping = kwargs.get("enable_mapping", False)

        if enable_mapping:
            predictions_ = cls.sigmoid_adjust_raw(predictions, groundtruths)
        else:
            predictions_ = predictions

        return groundtruths, predictions_

    @staticmethod
    def _confidence_curve(delta_vqm, z_vqm):
        cdf_z_vqm = 0.5 + scipy.special.erf(z_vqm / np.sqrt(2)) / 2
        vqm_bins = 10
        vqm_low = min(delta_vqm)
        vqm_high = max(delta_vqm)
        vqm_step = (vqm_high - vqm_low) / vqm_bins
        low_limits = np.arange(vqm_low, vqm_high - vqm_step, step=vqm_step / 2)
        centers = low_limits.copy() + vqm_step / 2
        high_limits = low_limits.copy() + vqm_step
        if high_limits[-1] < vqm_high:
            low_limits = np.hstack([low_limits, vqm_high - vqm_step])
            high_limits = np.hstack([high_limits, vqm_high])
            centers = np.hstack([centers, vqm_high - vqm_step / 2])

        assert len(centers) == len(low_limits) == len(high_limits)
        mean_confidence = np.zeros(len(centers))
        for index, (low_limit, high_limit) in enumerate(zip(low_limits, high_limits, strict=True)):
            in_bin = indices(
                delta_vqm,
                lambda value, low=low_limit, high=high_limit: low <= value < high,
            )
            mean_confidence[index] = float("NaN") if not in_bin else np.mean(cdf_z_vqm[in_bin])
        valid_points = [
            pair for pair in zip(centers, mean_confidence, strict=True) if not np.isnan(pair[1])
        ]
        return zip(*valid_points, strict=True)

    @staticmethod
    def _groundtruth_statistics(groundtruths, degrees_of_freedom):
        viewers = np.array(list(map(len, groundtruths)))
        means = np.array(list(map(np.nanmean, groundtruths)))
        deviations = np.array(
            [np.nanstd(groundtruth, ddof=degrees_of_freedom) for groundtruth in groundtruths]
        )
        return viewers, means, deviations**2

    @staticmethod
    def _upper_triangle_pairs(values):
        matrix = np.subtract.outer(values, values)
        return np.hstack(
            [matrix[0 : column - 1, column - 1] for column in range(2, len(values) + 1)]
        )

    @classmethod
    def _oriented_pair_statistics(cls, predictions, means, variance, viewers):
        prediction_differences = cls._upper_triangle_pairs(predictions)
        standard_error = variance / viewers
        standard_error = np.sqrt(standard_error[:, None] + standard_error[None, :])
        standard_error[standard_error == 0.0] = 1e-8
        z_matrix = np.subtract.outer(means, means) / standard_error
        z_scores = np.hstack(
            [z_matrix[0 : column - 1, column - 1] for column in range(2, len(means) + 1)]
        )
        negative = prediction_differences < 0
        prediction_differences[negative] *= -1
        z_scores[negative] *= -1
        return prediction_differences, z_scores

    @staticmethod
    def _interpolate_resolving_power(confidence, centers):
        try:
            return scipy.interpolate.interp1d(confidence, centers, kind="linear")([0.95])[0]
        except ValueError:
            return float("NaN")

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        if isinstance(groundtruths, (list, tuple)) and isinstance(groundtruths[0], dict):
            raise TypeError("{} cannot handle dictionary-style daataset yet.".format(cls.__name__))
        viewers, means, variance = cls._groundtruth_statistics(groundtruths, kwargs.get("ddof", 0))
        differences, z_scores = cls._oriented_pair_statistics(
            np.array(predictions), means, variance, viewers
        )
        centers, confidence = cls._confidence_curve(differences, z_scores)
        power = cls._interpolate_resolving_power(confidence, centers)
        return {"resolving_power_95perc": power, "score": power}


class AggrScorePerfMetric(PerfMetric):
    """
    Groundtruth is a list of aggregate scores (list of real numbers)
    """

    @staticmethod
    def sigmoid_adjust(xs, ys):
        ys_max = np.max(ys) + 0.1
        ys_min = np.min(ys) - 0.1

        # normalize to [0, 1]
        ys = list((np.array(ys) - ys_min) / (ys_max - ys_min))

        zs = -np.log(1.0 / np.array(ys).T - 1.0)
        Y_mtx = np.array((np.ones(len(ys)), zs)).T
        x_vec = np.array([xs]).T
        a_b = lstsq(Y_mtx, x_vec, rcond=-1)[0]
        a = a_b.item(0)
        b = a_b.item(1)

        xs = 1.0 / (1.0 + np.exp(-(np.array(xs) - a) / b))

        # denormalize
        return xs * (ys_max - ys_min) + ys_min

    @classmethod
    def _preprocess(cls, groundtruths, predictions, **kwargs):
        aggre_method = kwargs.get("aggr_method", np.mean)
        enable_mapping = kwargs.get("enable_mapping", False)

        groundtruths_ = [aggre_method(x) if hasattr(x, "__len__") else x for x in groundtruths]

        if enable_mapping:
            predictions_ = cls.sigmoid_adjust(predictions, groundtruths_)
        else:
            predictions_ = predictions

        return groundtruths_, predictions_


class RmsePerfMetric(AggrScorePerfMetric):

    TYPE = "RMSE"
    VERSION = "1.0"

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        rmse = np.sqrt(np.mean(np.power(np.array(groundtruths) - np.array(predictions), 2.0)))
        return {"score": rmse}


class SrccPerfMetric(AggrScorePerfMetric):

    TYPE = "SRCC"
    VERSION = "1.0"

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        # spearman
        srcc, _ = scipy.stats.spearmanr(groundtruths, predictions)
        return {"score": srcc}


class PccPerfMetric(AggrScorePerfMetric):

    TYPE = "PCC"
    VERSION = "1.0"

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        # pearson
        pcc, _ = scipy.stats.pearsonr(groundtruths, predictions)
        return {"score": pcc}


class KendallPerfMetric(AggrScorePerfMetric):

    TYPE = "KENDALL"
    VERSION = "1.0"

    @classmethod
    def _evaluate(cls, groundtruths, predictions, **kwargs):
        # kendall
        kendall, _ = scipy.stats.kendalltau(groundtruths, predictions)
        return {"score": kendall}
