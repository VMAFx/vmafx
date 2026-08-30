- Sanitizer matrix (ASan / UBSan / TSan in
  `tests-and-quality-gates.yml::sanitizers`) now runs the full C unit-test
  set under each leg, replacing the prior `meson test --suite=unit`
  invocation that matched zero tests because no `test()` call in
  `core/test/meson.build` carries a `suite: 'unit'` tag — every leg
  was previously printing `No suitable tests defined.` and exiting 0
  with zero correctness coverage. Per-sanitizer deselect lists
  document tests excluded because of real defects (not a sanitizer
  mis-configuration); each deselect corresponds to a tracked
  follow-up bug. UBSan additionally builds with
  `-fno-sanitize=function` to skip the K&R-prototype harness UB
  pattern in `core/test/test.h` + ~50 `test_*.c` files; library
  signal stays intact. Surfaces seven previously-hidden defects
  (svm.cpp malformed-JSON parse path, dict/extractor leaks in
  `test_predict` / `test_float_ms_ssim_min_dim`, integer_adm
  `div_lookup` global-init race, framesync mutex-domain mismatch).
  See [ADR-0347](docs/adr/0347-sanitizer-matrix-test-scope.md) and
  [research-0090](docs/research/0090-sanitizer-matrix-test-scope.md).
