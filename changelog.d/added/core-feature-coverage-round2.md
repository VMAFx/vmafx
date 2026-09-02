## Added

- **Feature-extractor coverage round 2** — 7 new test executables wired
  into `core/test/meson.build` plug uncovered branches in seven CPU-side
  files under `core/src/feature/` that sat in the 17 %-80 % range of the
  2026-05-31 Coverage Gate baseline (ADR-0114). Pure test-only addition;
  no production code changes.

  | File | Before | After | Test executable |
  |---|---:|---:|---|
  | `core/src/feature/integer_motion.c` | 71.8 % | 81.7 % | `test_integer_motion_coverage` |
  | `core/src/feature/integer_motion_v2.c` | 17.6 % | 88.2 % | `test_integer_motion_v2_coverage` |
  | `core/src/feature/integer_psnr.c` | 70.1 % | 92.0 % | `test_integer_psnr_coverage` |
  | `core/src/feature/integer_vif.h` | 72.2 % | 100.0 % | `test_integer_vif_log2` |
  | `core/src/feature/iqa/convolve.c` | 41.2 % | 98.0 % | `test_iqa_convolve_coverage` |
  | `core/src/feature/barten_csf_tools.h` | 45.5 % | 48.5 % | `test_barten_csf_coverage` |
  | `core/src/feature/ms_ssim_decimate.c` | 80.4 % | 95.7 % | `test_ms_ssim_decimate_coverage` |

  Coverage scope: option-driven init paths (`min_sse`, `enable_apsnr`,
  `motion_force_zero`, `motion_moving_average`, `motion_five_frame_window`),
  HBD extract paths (10 / 12 / 16-bit), multi-frame extract+flush flows,
  the `prev_ref`-driven `motion_v2` pipeline, the `integer_vif` log2 LUT
  inline helpers, the `iqa` boundary-extension helpers (`KBND_SYMMETRIC`
  / `KBND_REPLICATE` / `KBND_CONSTANT`), the legacy and MAE
  `barten_watson_blend_csf*` resolution dispatchers (8 paths plus the
  `-EINVAL` fallthrough), and the `ms_ssim_decimate` runtime-dispatch
  wrapper with NULL out-pointer handling.

  Follow-up to PR #344 (round 1); does not overlap with the four files
  PR #344 owns (`mkdirp.c`, `luminance_tools.c`, `feature_name.c`,
  `feature_extractor.c`). All 68 fast / simd / dnn suite tests pass.
