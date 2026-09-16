- **SYCL lint, second pass.** Three more families cleared. The `extern "C"` linkage band (ADR-0278 citation form) now
  covers the 12 SYCL translation units that lacked it — these `static`
  entry points cannot move into an anonymous namespace because their
  addresses live in an `extern "C" VmafFeatureExtractor` struct and a
  namespace may not appear inside a linkage specification, which is the
  same reason already recorded in `float_adm_sycl.cpp`.
  `modernize-use-integer-sign-comparison` is clean. And 19
  `bugprone-implicit-widening-of-multiplication-result` sites now do
  their work-group round-up in `size_t` instead of computing in 32-bit
  and widening the result.
  Two further checks were measured and left alone as unsound on this
  code: `readability-non-const-parameter` (33) and
  `misc-static-assert` (24), the latter converting runtime asserts on
  non-constant expressions into `static_assert` that does not compile.
  Verified after every step on the Arc A380: 195/195 SYCL tests, plus
  148/148 CPU.
