- SIMD bit-exactness round-2 fix (follow-up to PR #339):
  unify on FMA-based computation across both the SSIMULACRA 2
  AVX2/AVX-512 main-loop kernels and their scalar tails plus
  the `picture_to_linear_rgb` test reference. Under icx +
  `-mfma`, the prior `_mm256_add_ps(_, _mm256_mul_ps(_, _))`
  pattern was being auto-fused to FMA despite `-fp-model=precise`,
  while gcc kept it as two separately-rounded operations —
  any divergence between the scalar reference and SIMD path
  failed `test_ssimulacra2_simd::test_ptlr_420_8`. Forcing
  explicit `_mm256_fmadd_ps` / `_mm512_fmadd_ps` in the SIMD
  TUs plus `fmaf()` in the scalar tails + test reference
  unifies the rounding for every supported x86 compiler.
  Also extends the `-fp-model=precise` flag from #339 to
  `libvmaf_feature_static_lib` and `libvmaf_ssimulacra2_static_lib`
  so the scalar TUs inside those libs (notably
  `ms_ssim_decimate_scalar`, the reference for
  `test_ms_ssim_decimate`) get the same icx FP discipline
  as the carve-out SIMD libs (ADR-0891).
