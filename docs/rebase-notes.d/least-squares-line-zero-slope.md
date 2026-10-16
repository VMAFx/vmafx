## Python harness: the scatter-plot fit line keeps a zero slope (2026-10-16)

- `RegressorMixin._least_squares_line()` in
  `compat/python-vmaf/core/train_test_model.py` is fork code; upstream's
  `train_test_model.py` still calls `tools.misc.linear_fit()` there. It pads
  the coefficients of `Polynomial.fit(...).convert()` to two, because
  `convert()` trims a trailing zero slope. An upstream sync that touches the
  plotting code keeps the fork's helper and the padding;
  `python/test/least_squares_line_test.py` fails without it.
