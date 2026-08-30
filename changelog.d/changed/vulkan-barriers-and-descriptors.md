- **perf / vulkan**: Tighten `dstAccessMask` to `VK_ACCESS_SHADER_READ_BIT`
  only in five Vulkan extractor barriers where the consuming dispatch is
  read-only (`adm` inter-DWT stages, `float_adm` inter-DWT stages,
  `float_vif` inter-scale, `cambi` SAT-pipeline, `ssimulacra2` mul→blur /
  H→V blur).  The wider `SHADER_READ_BIT | SHADER_WRITE_BIT` mask is
  intentionally kept for the two-level GPU reducer barriers in
  `vif_vulkan.c`, `adm_vulkan.c`, and `motion_vulkan.c` because those
  reducer shaders use `atomicAdd` into `reduced_accum` / `reduced_sad`
  (ADR-0356 invariant — removing `SHADER_WRITE` from those sites would
  introduce a write-after-write hazard).  Fixes VK-5 from the
  2026-05-16 Vulkan perf-audit.

- **perf / vulkan**: Move `vkUpdateDescriptorSets` for `psnr_hvs_vulkan.c`
  from `extract()` to `init()`.  Buffer handles `ref_in` / `dist_in` /
  `partials` are pre-allocated and stable across frames; calling
  `vkUpdateDescriptorSets` every frame for all 3 planes was a hot-path
  overhead with no correctness purpose.  Mirrors the existing
  `psnr_vulkan.c` / `ssim_vulkan.c` / `ciede_vulkan.c` pattern.
  Fixes VK-6 from the 2026-05-16 Vulkan perf-audit.
