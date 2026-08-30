## Added

- **Metal kernel parity tests round 3** (`core/test/test_metal_integer_motion_parity.c`,
  `core/test/test_metal_float_motion_parity.c`,
  `core/test/test_metal_float_moment_parity.c`,
  `core/test/test_metal_float_ms_ssim_parity.c`): four CPU-vs-Metal
  score-comparison tests covering the remaining four Metal extractors that
  PR #351 (registration audit) and PR #379 (round-2 parity: motion_v2,
  integer_psnr, float_psnr, float_ssim) did not exercise numerically. With
  this round all 8 registered Metal extractors now have a real per-kernel
  parity gate. Each test runs a synthetic 256x144 YUV420P fixture through
  the CPU twin and the Metal extractor; asserts places=4 (1e-4) parity per
  ADR-0214, except `float_ms_ssim` which uses the 1e-3 SSIM-family bound
  from ADR-0589. Skips cleanly via `-ENODEV` when `vmaf_metal_state_init`
  fails on Linux / Windows / Intel Mac. Wired into `core/test/meson.build`
  under the existing `enable_metal` guard with `suite : ['fast', 'gpu']`.
