- Six HIP feature extractors (`float_vif_hip`, `adm_hip`,
  `integer_ms_ssim_hip`, `psnr_hvs_hip`, `integer_ssim_hip`,
  `ssimulacra2_hip`) are now resolvable via
  `vmaf_get_feature_extractor_by_name(<name>)` and selectable on the
  CLI with `--feature <name>`. The TUs already shipped under
  `core/src/feature/hip/` but were missing from `hip_sources`
  and from the `extern` + registry blocks in `feature_extractor.c`,
  so the registry returned NULL. Generalises the ADR-0523 single-
  extractor fix to the rest of the HIP-extractor inventory. Scaffold
  posture preserved — `init()` returns `-ENOSYS` unless built with
  `enable_hipcc=true` for the scaffold TUs (ADR-0533).
