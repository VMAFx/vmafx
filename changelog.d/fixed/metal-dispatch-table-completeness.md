- **Metal dispatch table completeness.** `vmaf_metal_dispatch_supports()`
  recognised only 7 of the 17 registered Metal extractors, so the other 10
  (`integer_ssim`, `float_vif`, `integer_vif`, `float_adm`, `integer_adm`,
  `integer_ciede`, `integer_psnr_hvs`, `integer_cambi`, `ssimulacra2`, plus the
  `float_ms_ssim_metal` registry name) returned 0 and silently fell back to CPU
  even on Apple Silicon. Added every missing registry name and provided-features
  key to `g_metal_features[]` in `core/src/metal/dispatch_strategy.c`, verified
  against each `.mm`'s `provided_features[]` source-of-truth. Updated the
  stale `core/test/test_metal_kernel_coverage_audit.c` (basenames 8 → 17,
  `EXPECTED_KERNEL_COUNT` 8 → 17) and replaced its now-shipped phantom names
  (`vif_metal` / `adm_metal` / `ciede2000_metal` / `ssimulacra2_metal`) with
  genuinely non-existent names so the wildcard-regression guard stays honest.
