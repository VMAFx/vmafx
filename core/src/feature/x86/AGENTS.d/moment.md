---
paths:
  - core/src/feature/x86/moment_avx2.c
  - core/src/feature/x86/moment_avx512.c
  - core/src/feature/float_moment.c
invariant: Every lane is added into one double in raster order; scalar bits on every input (ADR-1500).
---
# float_moment SIMD Reduction Invariants

| Group | TUs that move in lockstep |
| --- | --- |
| **float_moment SIMD** (ADR-0179 / ADR-0987, [ADR-1500](../../../../../docs/adr/1500-arm-float-moment-scalar-order.md)) | `moment_avx2.c` + `moment_avx512.c` + `../arm64/moment_neon.c` + `../arm64/moment_sve2.c` + scalar `../moment.c` (dispatch in `../float_moment.c`). Every kernel squares in `float` and adds each lane into one `double` one after other, in raster order: scalar's bits on every input. `../../test/test_moment_simd.c` asserts `==` on frames whose sum passes 2^53 units, where any other grouping rounds differently (1e-7 tolerance of ADR-0179 / ADR-0987 is superseded). AVX-512 path (`HAVE_AVX512` gate, `compute_1st/2nd_moment_avx512`) stores 16 lanes per step instead of 8. Do NOT add lanes into separate accumulators or reduce them in vector. |
