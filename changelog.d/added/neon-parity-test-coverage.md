- Eight new NEON-vs-scalar parity tests close the coverage gap that let ADR-1057
  reach master: `test_vif_neon`, `test_ssim_neon`, `test_float_adm_neon`,
  `test_float_adm_dwt2_neon`, `test_motion_neon`, `test_float_motion_neon`,
  `test_psnr_neon` and `test_ciede_neon`. Together with `test_adm_dwt2_neon`,
  every previously-uncovered `core/src/feature/arm64/` kernel file now has a
  bit-exactness test against its scalar reference, run under
  `qemu-aarch64-static` in the `fast`/`simd` suite (106/106 on aarch64).
- Each test sweeps geometries chosen to exercise the boundary the kernel's
  vector stride creates — widths that are and are not multiples of the stride,
  odd heights — because that is where every defect found in this pass lived.
