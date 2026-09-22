__copyright__ = "Copyright 2016-2020, Netflix, Inc."
__license__ = "BSD+Patent"

import numpy as np


class ListStats(object):
    """
    >>> test_list = [1, 2, 3, 4, 5, 11, 12, 13, 14, 15]
    >>> "%0.4f" % ListStats.total_variation(test_list)
    '1.5556'
    >>> float(np.mean(test_list))
    8.0
    >>> float(np.median(test_list))
    8.0
    >>> float(ListStats.lp_norm(test_list, 1.0))
    8.0
    >>> "%0.4f" % ListStats.lp_norm(test_list, 3.0)
    '10.5072'
    >>> "%0.2f" % ListStats.perc1(test_list)
    '1.09'
    >>> "%0.2f" % ListStats.perc5(test_list)
    '1.45'
    >>> "%0.2f" % ListStats.perc10(test_list)
    '1.90'
    >>> "%0.2f" % ListStats.perc20(test_list)
    '2.80'
    >>> float(ListStats.nonemean([None, None, 1, 2]))
    1.5
    >>> float(ListStats.nonemean([3, 4, 1, 2]))
    2.5
    >>> float(ListStats.nonemean([None, None, None]))
    nan
    >>> "%0.4f" % ListStats.harmonic_mean(test_list)
    '4.5223'
    >>> "%0.4f" % ListStats.lp_norm(test_list, 2.0)
    '9.5394'
    """

    @staticmethod
    def total_variation(my_list):
        abs_diff_scores = np.absolute(np.diff(my_list))
        return np.mean(abs_diff_scores)

    @staticmethod
    def moving_average(my_list, n, type="exponential", decay=-1):
        """
        compute an n period moving average.
        :param my_list:
        :param n:
        :param type: 'simple' | 'exponential'
        :param decay:
        :return:
        """
        x = np.asarray(my_list)
        if type == "simple":
            weights = np.ones(n)
        elif type == "exponential":
            weights = np.exp(np.linspace(decay, 0.0, n))
        else:
            assert False, "Unknown type: {}.".format(type)

        weights /= weights.sum()

        a = np.convolve(x, weights, mode="full")[: len(x)]
        a[:n] = a[n]
        return a

    @staticmethod
    def harmonic_mean(my_list):
        return 1.0 / np.mean(1.0 / (np.array(my_list) + 1.0)) - 1.0

    @staticmethod
    def lp_norm(my_list, p):
        return np.power(np.mean(np.power(np.array(my_list), p)), 1.0 / p)

    @staticmethod
    def perc1(my_list):
        return np.percentile(my_list, 1)

    @staticmethod
    def perc5(my_list):
        return np.percentile(my_list, 5)

    @staticmethod
    def perc10(my_list):
        return np.percentile(my_list, 10)

    @staticmethod
    def perc20(my_list):
        return np.percentile(my_list, 20)

    @staticmethod
    def print_stats(my_list):
        print(
            "Min: {min}, Max: {max}, Median: {median}, Mean: {mean},"
            " Variance: {var}, Total_variation: {total_var}".format(
                min=np.min(my_list),
                max=np.max(my_list),
                median=np.median(my_list),
                mean=np.mean(my_list),
                var=np.var(my_list),
                total_var=ListStats.total_variation(my_list),
            )
        )

    @staticmethod
    def print_moving_average_stats(my_list, n, type="exponential", decay=-1):
        moving_avg_list = ListStats.moving_average(my_list, n, type, decay)
        ListStats.print_stats(moving_avg_list)

    @staticmethod
    def nonemean(my_list):
        # np.mean([]) is NaN, but it reaches that answer by warning
        # "Mean of empty slice" first. The harness runs under
        # `filterwarnings = error`, so an all-None input has to produce the
        # same NaN without the warning.
        values = [x for x in my_list if x is not None]
        if not values:
            return np.float64("nan")
        return np.mean(values)


def vectorized_gaussian(xs, locs, scales):
    """Gaussian density that stays defined where ``scales`` is exactly zero.

    ``sureal.tools.stats.vectorized_gaussian`` evaluates
    ``1 / sqrt(2*pi) / scale * exp(-(x - loc)**2 / (2 * scale**2))`` directly.
    A stimulus that every observer rated identically has a sample standard
    deviation of exactly zero, so ``scale`` is zero: the leading division
    yields ``inf``, the exponent's ``0 / 0`` yields ``nan``, and ``inf * nan``
    is ``nan``.  NumPy reports both steps as ``RuntimeWarning``, and the
    resulting ``nan`` is then dropped by the ``numpy.nansum`` in sureal's
    log-likelihood, which still divides the truncated sum by the full
    observation count.  The reported log-likelihood, AIC and BIC are therefore
    silently computed over a subset of the observations.

    The zero-width limit of a Gaussian is the Dirac delta: the density is
    unbounded at ``x == loc`` and zero everywhere else.  This implementation
    returns that limit, so a dataset containing a unanimously rated stimulus
    reports an infinite log-likelihood -- the honest statement that its
    maximum likelihood is unbounded -- rather than a quietly under-counted
    finite number.  ``nan`` observations keep propagating as ``nan``, exactly
    as they do on the finite-scale branch, so sureal's ``nansum`` still skips
    missing ratings.

    Every finite non-zero scale is evaluated by the original expression, so
    non-degenerate inputs are bit-identical to sureal's.

    >>> float(vectorized_gaussian(np.array([0.0]), np.array([0.0]), np.array([1.0]))[0])
    0.3989422804014327
    >>> vectorized_gaussian(np.array([1.0, 2.0]), np.array([1.0, 1.0]), np.array([0.0, 0.0]))
    array([inf,  0.])
    """
    xs, locs, scales = np.broadcast_arrays(
        np.asarray(xs, dtype=float),
        np.asarray(locs, dtype=float),
        np.asarray(scales, dtype=float),
    )
    degenerate = scales == 0.0
    if not degenerate.any():
        return 1.0 / np.sqrt(2 * np.pi) / scales * np.exp(-((xs - locs) ** 2) / (2 * scales**2))

    densities = np.empty(np.shape(xs), dtype=float)
    spread = ~degenerate
    spread_scales = scales[spread]
    densities[spread] = (
        1.0
        / np.sqrt(2 * np.pi)
        / spread_scales
        * np.exp(-((xs[spread] - locs[spread]) ** 2) / (2 * spread_scales**2))
    )
    offsets = xs[degenerate] - locs[degenerate]
    densities[degenerate] = np.where(
        np.isnan(offsets), np.nan, np.where(offsets == 0.0, np.inf, 0.0)
    )
    return densities


def patch_sureal_vectorized_gaussian():
    """Bind :func:`vectorized_gaussian` into ``sureal`` in place of its own.

    sureal 0.9.0 is the newest release and still divides by a zero standard
    deviation, which the harness's fatal-warning policy turns into a test
    failure whenever a dataset contains a unanimously rated stimulus.  There is
    no upstream release to upgrade to and no extension point to register a
    replacement through, so the corrected density is bound over the name in the
    module that defines it and in ``sureal.subjective_model``, which imported
    it by value at its own import time.  The call is idempotent and must run
    before any subjective model is fitted.
    """
    import sureal.subjective_model
    import sureal.tools.stats

    sureal.tools.stats.vectorized_gaussian = vectorized_gaussian
    sureal.subjective_model.vectorized_gaussian = vectorized_gaussian


if __name__ == "__main__":
    import doctest

    doctest.testmod()
