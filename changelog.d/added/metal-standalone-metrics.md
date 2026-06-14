- **Metal standalone-metric kernels — Metal parity sweep COMPLETE (9/9).** The
  Metal backend now implements `integer_ciede` (CIEDE2000), `integer_psnr_hvs`,
  `integer_cambi` (banding; Strategy-II hybrid per ADR-0205 — GPU mask/decimate/
  filter + exact-CPU host residual, bit-identical to the CPU `cambi`), and
  `ssimulacra2`. With the earlier ssim/vif/adm/motion kernels, every CPU feature
  extractor with a CUDA/SYCL twin now has a Metal twin. Each is parity-checked vs
  the CPU reference on the macOS Apple-Silicon CI lane (places=4, ADR-0214).
