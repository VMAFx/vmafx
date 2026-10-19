# Copyright 2026 Lusoris
# SPDX-License-Identifier: EUPL-1.2
"""Tests for ``RegressorMixin._least_squares_line``, the scatter-plot fit line.

Fork-local tests; the Netflix golden assertions are untouched (CLAUDE.md §8).
"""

from __future__ import annotations

import unittest

import numpy as np

from vmaf.core.train_test_model import RegressorMixin


class LeastSquaresLineTest(unittest.TestCase):
    def test_line_through_points(self) -> None:
        slope, intercept = RegressorMixin._least_squares_line([0, 1, 2], [1, 3, 5])
        self.assertAlmostEqual(slope, 2.0)
        self.assertAlmostEqual(intercept, 1.0)

    def test_exact_zero_slope_keeps_two_values(self) -> None:
        # Polynomial.convert() trims the zero slope to a single coefficient here.
        self.assertEqual(np.polynomial.Polynomial.fit([0, 1], [0, 0], 1).convert().coef.size, 1)
        self.assertEqual(RegressorMixin._least_squares_line([0, 1], [0, 0]), (0.0, 0.0))

    def test_one_distinct_abscissa_has_no_line(self) -> None:
        self.assertIsNone(RegressorMixin._least_squares_line([2, 2], [1, 5]))


if __name__ == "__main__":
    unittest.main()
