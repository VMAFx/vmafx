- Python harness: the per-content fit lines of the subjective-dataset scatter plots
  (`RegressorMixin._least_squares_line()`) no longer raise
  `ValueError: not enough values to unpack` when a group's least-squares line has a
  slope of exactly zero. NumPy's `Polynomial.convert()` trims a trailing zero
  coefficient, so such a line came back as one coefficient; it is now padded back to
  intercept and slope.
