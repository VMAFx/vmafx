- Repaired test-build identity seams behind 25 open CodeQL
  `cpp/unused-static-function` findings. Redundant `pdjson.c` and
  `thread_pool.c` compilations were removed, the three orphan picture-test
  targets now share `test_picture_impl` while the pool error-path target keeps
  compiling `picture_pool.c` directly, intentional source-based test copies now
  isolate their private helper identities in test-local libraries, and the
  allocation-only `vector_unchanged` helper is emitted only with its non-LTO
  test harness. The production ABI and runtime behavior are unchanged; hosted
  closure remains pending a fresh default-branch analysis.
- Removed the remaining predictor identity seam: `test_predict.c` now consumes
  exact-order mapping helpers from `predict_internal.h` and links the production
  predictor instead of text-including `predict.c`. Its non-finite regression
  verifies the shipped warning path rather than a test-only logging macro.
  Meson now compiles `predict.c` once and links that source-authority archive
  into libvmaf and private-source tests, replacing 53 independently compiled
  identities. The previously reported CAMBI row was absent from the fresh
  exact-base query.
