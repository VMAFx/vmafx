- **`float_vif` no longer scores differently with and without SIMD.** ADR-1207's
  `test_feature_isa_invariance` drives the public API twice — once on the host's
  real ISA, once with `cpumask` disabling every SIMD flag — and asserts
  bit-identical scores. Its first run under clang failed:
  `VMAF_feature_vif_scale0_score` was `0.24375427845259967` with SIMD against
  `0.24375426056033248` without, a delta of **1.789e-08** and a breach of the
  ADR-0891 bit-exactness contract. The test had only ever run under gcc, which
  does not contract at the offending site, so a gcc-only run could not see it.
  The offender is the trio of `FORCE_INLINE` border helpers in
  `convolution_internal.h` — `convolution_edge_s`, `_sq_s` and `_xy_s` — that
  the AVX and AVX-512 convolutions inline to compute their border pixels. Each
  accumulated as `accum += filter[k] * src[...]`, a single expression a compiler
  may contract into one fused multiply-add, while the SIMD interior of the very
  same convolution uses explicit `_mm*_mul_ps` followed by `_mm*_add_ps` — two
  roundings. clang contracted the borders at every optimisation level, so a SIMD
  run's border pixels disagreed with a scalar run's. Each product is now
  materialised into a named `float` before the add, which C requires to discard
  extra precision, pinning both roundings in the source instead of in a build
  flag that a future toolchain change could drop. **No shipped score moves**:
  compiled non-LTO with gcc before and after, the instruction streams are
  byte-identical at 1,900 instructions with zero FMA.
