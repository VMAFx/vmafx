- `BrisqueNorefFeatureExtractor.extract_aggd_features` no longer flattens its
  patch by assigning to `ndarray.shape`, which NumPy 2.5 deprecates. The
  replacement (`np.reshape` on the C-contiguous copy) reads the same elements
  in the same order, so the AGGD fit — and the NIQE scores built on it — are
  bit-identical. With the Python harness now running under
  `filterwarnings = error`, the deprecation was failing
  `quality_runner_test::test_run_niqe_runner` in the Coverage Gate.
