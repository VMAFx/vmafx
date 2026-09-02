- The nine round-3/round-4 Metal feature extractors (`integer_ssim_metal`,
  `float_vif_metal`, `integer_vif_metal`, `float_adm_metal`,
  `integer_adm_metal`, `integer_ciede_metal`, `integer_psnr_hvs_metal`,
  `integer_cambi_metal`, `ssimulacra2_metal`) are registered again, so
  `vmaf_get_feature_extractor_by_name()` / `--feature <name>` resolve them on
  macOS Metal builds instead of returning NULL. Same PR #875 `.c` -> `.cpp`
  registry-split orphan that PR #1004 fixed for the GPU SpEED twins: the nine
  entries had been added to the dead `feature_extractor.c` twin, so deleting it
  dropped them from the compiled registry while the `.mm` kernels, the
  `g_metal_features[]` dispatch rows and the parity tests all stayed in place.
  Restores the 17/17 Metal coverage contract asserted by
  `test_metal_kernel_coverage_audit` (ADR-0959), which was the single red test
  on both macOS CI build legs. Non-Metal builds are unaffected — the block is
  inside `#if HAVE_METAL`.
