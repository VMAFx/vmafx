## Added

- **Test coverage uplift on the vendored libsvm 3.24 runtime API and
  the IQA helper tree** (`core/test/test_svm_api.c` +
  `core/test/test_iqa_helpers.c`, 29 new assertions across 2 new
  fast-suite executables). Observation-only against the existing
  public APIs; no vendored source is modified. Coverage moves from
  **9.6% → 71%** on `core/src/svm.cpp` and from **0–41% → 84–100%**
  across `math_utils.c` / `decimate.c` / `convolve.c` /
  `ssim_tools.c` (aggregate: ≈14% → 74%). Complements PR #381's
  `test_svm_parser.c` (parser-rejection paths) by exercising
  `svm_train`, `svm_predict`, `svm_predict_values`,
  `svm_predict_probability`, the inspector family,
  `svm_check_parameter` rejection branches, a `svm_save_model` →
  `svm_load_model` round-trip, plus every public symbol in the IQA
  helper bodies including `iqa_filter_pixel`, `iqa_img_filter`
  (NULL-bnd_opt rejection + in-place), `iqa_decimate` (factor=2 +
  odd dimension), all three `KBND_*` border handlers, and a
  full `iqa_ssim` end-to-end on identical and random frames that
  drives the scalar precompute / variance / accumulate fallback
  paths of `ssim_tools.c`. ADR-0952.
