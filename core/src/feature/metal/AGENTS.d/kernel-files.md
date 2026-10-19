---
paths:
  - core/src/feature/metal/*.mm
  - core/src/feature/metal/*.metal
invariant: table of every Metal kernel file, its status and features it emits.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Kernel files

| File                               | Status      | Feature(s)                                                              |
|------------------------------------|-------------|-------------------------------------------------------------------------|
| `integer_motion_v2.metal`          | Done (T8-1c) | `VMAF_integer_feature_motion_v2_sad_score`, `motion2_v2_score`         |
| `integer_motion_v2_metal.mm`       | Done (T8-1c) | host dispatch                                                           |
| `float_psnr.metal`                 | Done (T8-1d) | `float_psnr`                                                            |
| `float_psnr_metal.mm`              | Done (T8-1d) | host dispatch                                                           |
| `float_moment.metal`               | Done (T8-1e) | `float_moment_ref1st`, `float_moment_dis1st`, `float_moment_ref2nd`, `float_moment_dis2nd` |
| `float_moment_metal.mm`            | Done (T8-1e) | host dispatch (fixes provided_features)                                 |
| `integer_psnr.metal`               | Done (T8-1g) | `psnr_y`, `psnr_cb`, `psnr_cr`                                          |
| `integer_psnr_metal.mm`            | Done (T8-1g) | host dispatch                                                           |
| `float_motion.metal`               | Done (T8-1h) | `float_motion`                                                          |
| `float_motion_metal.mm`            | Done (T8-1h) | host dispatch                                                           |
| `integer_motion.metal`             | Done (T8-1i) | `VMAF_integer_feature_motion_sad_score`, `_motion_score`, `_motion2_score`, `_motion3_score` (ADR-1498) |
| `integer_motion_metal.mm`          | Done (T8-1i) | host dispatch                                                           |
| `float_ssim.metal`                 | Done (T8-1j) | `float_ssim`, `float_ssim_l`, `float_ssim_c`, `float_ssim_s`           |
| `float_ssim_metal.mm`              | Done (T8-1j) | host dispatch                                                           |
| `float_ms_ssim.metal`              | Done (T8-2b) | `float_ms_ssim`, `float_ms_ssim_cb`, `float_ms_ssim_cr` — 5-scale pyramid, Wang weights |
| `float_ms_ssim_metal.mm`           | Done (T8-2b) | host dispatch (ADR-0490, ADR-1334; enable_db, clip_db, enable_chroma)   |
| `integer_ssim.metal`               | Done         | `ssim`                                                                  |
| `integer_ssim_metal.mm`            | Done         | host dispatch                                                           |
| `float_vif.metal`                  | Done         | `VMAF_feature_vif_scale0..3_score`, `vif`, `vif_num/den` (+ per-scale) |
| `float_vif_metal.mm`               | Done         | host dispatch                                                           |
| `integer_vif.metal`                | Done         | `VMAF_integer_feature_vif_scale0..3_score`, `integer_vif` (+ per-scale)|
| `integer_vif_metal.mm`             | Done         | host dispatch                                                           |
| `float_adm.metal`                  | Done         | `VMAF_feature_adm2/aim/adm3/adm_scale0..3_score`, `adm_num/den` (+scale)|
| `float_adm_metal.mm`               | Done         | host dispatch                                                           |
| `integer_adm.metal`                | Done         | `VMAF_integer_feature_adm2/aim/adm3_score`, `integer_adm` (+ per-scale)|
| `integer_adm_metal.mm`             | Done         | host dispatch                                                           |
| `integer_ciede.metal`              | Done         | `ciede2000`                                                             |
| `integer_ciede_metal.mm`           | Done         | host dispatch                                                           |
| `integer_psnr_hvs.metal`           | Done         | `psnr_hvs_y`, `psnr_hvs_cb`, `psnr_hvs_cr`, `psnr_hvs`                  |
| `integer_psnr_hvs_metal.mm`        | Done         | host dispatch                                                           |
| `integer_cambi.metal`              | Done         | `Cambi_feature_cambi_score`                                            |
| `integer_cambi_metal.mm`           | Done         | host dispatch                                                           |
| `ssimulacra2.metal`                | Done         | `ssimulacra2`                                                           |
| `ssimulacra2_metal.mm`             | Done         | host dispatch                                                           |
