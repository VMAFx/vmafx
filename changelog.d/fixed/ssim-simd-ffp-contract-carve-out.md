- **`ssim_avx2.c` + `ssim_neon.c` — bit-exactness time-bombs
  via auto-FMA on `mul+mul+add`.** Both files compute
  `l_den = add(add(mul(rm,rm), mul(cm,cm)), vC1)` inside their
  respective main static libs (`x86_avx2_static_lib`,
  `arm64_static_lib`) which are built with `-mfma` (x86) or default
  `-ffp-contract=fast` (aarch64) and no `-ffp-contract=off`. The
  scalar reference in `ssim_tools.c` uses unfused mul+add, so the
  outer add+mul auto-contracts to `vfmadd.../vfmla.../fmla` and
  silently drifts vs scalar. No parity test currently catches the
  drift (`test_ssimulacra2_simd.c` covers SSIMULACRA2 only).
  Fix: extract both files into dedicated static libs
  (`x86_ssim_avx2_lib`, `arm64_ssim_neon_lib`) with
  `-ffp-contract=off`, mirroring the `ssimulacra2` / `psnr_hvs` /
  `ms_ssim_decimate` carve-outs landed in PR #282. Identified by
  simd-reviewer agent audit 2026-05-30 (HIGH severity).
