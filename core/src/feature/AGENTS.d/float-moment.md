---
paths:
  - core/src/feature/moment.c
  - core/src/feature/moment.h
  - core/src/feature/arm64/moment_neon.c
  - core/src/feature/arm64/moment_sve2.c
invariant: compute_2nd_moment adds float squares into one double in raster order; every SIMD kernel returns its bits.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# moment.c compute_2nd_moment Reduction Contract

- **`moment.c::compute_2nd_moment` reduction contract** (ADR-0179 / ADR-0987,
  [ADR-1500](../../../../docs/adr/1500-arm-float-moment-scalar-order.md)):
  `const float term = pic_ * pic_; cum += (double)term;` where float squaring
  is evaluated in single precision and explicitly cast to `double` before
  accumulation into `cum`, one sample after other in raster order. Every
  SIMD kernel (`x86/moment_avx2.c`, `x86/moment_avx512.c`,
  `arm64/moment_neon.c`, `arm64/moment_sve2.c`) adds its lanes into one
  `double` in that order and returns these bits on every input; past 2^53
  units sum rounds on every add, so any other grouping is another number.
  `test_moment_simd` asserts `==` for every kernel build has (1e-7
  tolerance of ADR-0179 / ADR-0987 is superseded). Decoupling
  float product into intermediate `term` and explicit `(double)` cast
  removes CodeQL `cpp/integer-multiplication-cast-to-long` source pattern
  behind historically dismissed Alert 707 (
  [Research-2031](../../../../docs/research/2031-codeql-float-widening-multiplication.md))
  by eliminating compiler-generated widening conversions. **On rebase:** do not
  pre-widen operands (`(double)pic_ * pic_`), revert to direct implicit
  widening (`cum += pic_ * pic_`) or reorder adds; change to this loop
  changes every SIMD kernel in same PR.
