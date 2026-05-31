### Fixed

- **ARM64 NEON float TUs now compiled with `-ffp-contract=off`** (`arm64_v8_fp` static lib).
  Previously `float_adm_neon.c`, `float_psnr_neon.c`, `float_motion_neon.c`,
  `ms_ssim_decimate_neon.c`, `convolve_neon.c`, `psnr_hvs_neon.c`, `moment_neon.c`,
  and `motion_v2_neon.c` were compiled in the `arm64_v8` lib which lacked the flag.
  GCC ≥ 10 / Clang on aarch64 default to `-ffp-contract=fast`, which can auto-fuse
  `a*b+c` in plain C into `fmla` instructions, producing results that diverge from
  the scalar reference (ADR-0873).
- **`float_adm_neon.c` wired into the dispatch path** on ARM64 — the three
  `float_adm_*_neon` functions were previously compiled but never called (dead code).
- **`float_adm_sum_cube_neon` and `float_adm_csf_den_scale_neon` reduction stability**:
  float32 accumulator trees converted to double via `vcvt_f64_f32`, matching the
  AVX2 `_mm256_cvtps_pd` precedent (ADR-0138).
- **NEON arm added to `test_motion_v2_simd.c`**: `motion_score_pipeline_16_neon`
  is now exercised against the scalar reference on the adversarial negative-diff
  and mixed-diff fixtures on `ubuntu-24.04-arm` CI runners.
