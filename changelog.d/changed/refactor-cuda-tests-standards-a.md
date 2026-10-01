- **Refactor CUDA test files part 1 for clang-tidy and HISS standard compliance (ADR-1142).**
  Brings 11 CUDA test files (`test_cuda_pic_preallocation.c`, `test_cuda_float_adm_parity.c`,
  `test_cuda_motion3_parity.c`, `test_cuda_psnr_parity.c`, `test_cuda_float_ms_ssim_parity.c`,
  `test_cuda_float_moment_parity.c`, `test_cuda_float_psnr_parity.c`, `test_cuda_speed_chroma_parity.c`,
  `test_cuda_ciede_parity.c`, `test_cuda_motion_v2_parity.c`, `test_cuda_preallocation_leak.c`)
  to 0 warnings in the `cuda` lane, tightening the baseline by 250 warnings (from 1149 to 899).
  Applies file-level ADR-1138 `modernize-use-nullptr` brackets, isolates variable declarations,
  and extracts helpers to satisfy function size and branch complexity constraints.
