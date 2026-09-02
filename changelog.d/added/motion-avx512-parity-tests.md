- **test**: add `test_motion_avx512_parity` — direct bit-exact unit tests for
  all six AVX-512 motion kernels (`motion_score_pipeline_8_avx512`,
  `motion_score_pipeline_16_avx512`, `sad_avx512`, `y_convolution_8_avx512`,
  `y_convolution_16_avx512`, `x_convolution_16_avx512`) against their scalar
  references; closes coverage gap flagged by the SIMD audit.  Also adds
  `simd_test_have_avx512()` to `simd_bitexact_test.h` for reuse by future
  AVX-512 tests.  Gated on `VMAF_X86_CPU_FLAG_AVX512`; skips cleanly on
  hosts without AVX-512 (ADR-0854).
