## Added

- **Metal kernel parity tests round 2** (`core/test/test_metal_motion_v2_parity.c`,
  `core/test/test_metal_integer_psnr_parity.c`,
  `core/test/test_metal_float_psnr_parity.c`,
  `core/test/test_metal_float_ssim_parity.c`): four CPU-vs-Metal score-comparison
  tests on synthetic 256x144 YUV420P fixtures that extend the registration audit
  added in PR #351 (which only asserted extractor discoverability) with real
  numerical parity gates. Each test skips cleanly via `-ENODEV` when
  `vmaf_metal_state_init` fails on Linux / Windows / Intel Mac, and asserts
  places=4 (1e-4) parity per ADR-0214 on Apple-Family-7+ macOS CI lanes —
  except `float_ssim` which uses the 1e-3 SSIM-specific bound from ADR-0589.
  Wired into `core/test/meson.build` under the existing `enable_metal` guard
  with `suite : ['fast', 'gpu']`. Closes the Metal-kernel coverage gap that
  PR #351 left open (per its own follow-up note).
