- **Metal standalone-metric kernels — standalone-metrics sweep complete (9/9 in
  that batch).** The Metal backend now implements `integer_ciede` (CIEDE2000),
  `integer_psnr_hvs`, `integer_cambi` (banding; Strategy-II hybrid per ADR-0205,
  GPU mask/decimate/filter + exact-CPU host residual, bit-identical to the CPU
  `cambi`), and `ssimulacra2`. Together with the earlier ssim/vif/adm/motion
  kernels the Metal backend now ships 17 wired, registered, parity-tested
  extractors. One known Metal-twin gap remains: the SpEED family
  (`speed_chroma` / `speed_temporal`) has CUDA/SYCL/HIP twins but no Metal kernel.
  Each shipped kernel is parity-checked vs the CPU reference on the macOS
  Apple-Silicon CI lane (places=4, ADR-0214).
