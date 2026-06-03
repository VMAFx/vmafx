- **CUDA ms_ssim float precision fix (ADR-0990)**: `ms_ssim_vert_lcs`
  kernel now computes per-pixel L/C/S and warp/block reductions in
  `double` instead of `float`, matching the CPU scalar reference in
  `ssim_tools.c` which uses `2.0 *` double-precision literals. The
  `float` accumulation caused ~0.004 drift over 33k pixels at scale 0,
  40x the places=4 (1e-4) tolerance gate. Fixes
  `test_cuda_float_ms_ssim_parity`. Applies the ADR-0139 pattern
  (previously fixed for AVX2/AVX-512) to the CUDA backend.
  Blamed commit: `8db2715ac2`.
