- Remove the bundled libsvm predictor's file-wide analyzer cordon and fix the
  544 strict findings it exposed. Allocation arithmetic is checked, parser and
  model ownership unwind cleanly, model writes are verified and always closed,
  solver virtual dispatch is explicit, and cross-validation no longer uses
  process-global `rand()` state.
- Eliminate 65 host-compiler diagnostics found across feature-context growth,
  model and SSIMULACRA2 tests, integer PSNR, SSIMULACRA2 color conversion,
  libsvm, the feature-extractor pool, and SYCL CLI setup. No warning was
  suppressed and no lint, HISS, golden-score, or tolerance baseline changed.
- Remove 35 real HISS findings from the touched feature, CUDA SpEED,
  SSIMULACRA2 and CLI paths. The CLI now has structured resource ownership and
  no cleanup jumps or function-size exceptions; strict CUDA clang-tidy is clean
  for both `speed_chroma_cuda.c` and `vmaf.cpp`. CUDA CLI runs now retain and
  release the state allocation after `vmaf_close()`, and cleanup/output
  failures are observable instead of being discarded.
- Keep the CUDA gpumask dispatch smoke test within its hang-detector budget by
  using a 576x324 two-frame fixture instead of four 1920x1080 scoring runs.
  Backend-selection, temporal-model and PSNR variants remain covered.
