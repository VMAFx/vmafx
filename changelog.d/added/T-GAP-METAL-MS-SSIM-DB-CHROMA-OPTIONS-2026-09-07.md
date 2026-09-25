- **Metal `float_ms_ssim` option and score parity (T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07 / ADR-1221).**
  The Metal `float_ms_ssim` feature extractor (`core/src/feature/metal/float_ms_ssim_metal.mm`)
  now exposes `enable_db`, `clip_db`, and `enable_chroma` alongside `enable_lcs`,
  reaching full parity with the CPU reference and shipped GPU twins. When `enable_chroma`
  is set, Metal computes `float_ms_ssim_cb` and `float_ms_ssim_cr` across 3 planes on the GPU
  and advertises them in `provided_features` and `core/src/metal/dispatch_strategy.c`.
  Subsampled chroma requires at least 176x176 dimensions (352x352 luma for YUV420P),
  enforced at init. When `enable_db` and `clip_db` are set, scores are clipped at the
  frame geometry `max_db` ceiling (`ceil(10 * log10(peak² / mse))`). Device-free
  contract tests (`core/test/test_metal_ms_ssim_options_contract.py`) and updated
  Metal parity tests (`core/test/test_metal_float_ms_ssim_parity.c`) verify the options,
  geometry ceiling, and chroma channels.
