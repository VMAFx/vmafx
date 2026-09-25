- **Metal `float_ms_ssim` option and score parity (T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07 / ADR-1334).**
  The Metal `float_ms_ssim` feature extractor (`core/src/feature/metal/float_ms_ssim_metal.mm`)
  now exposes `enable_db`, `clip_db`, and `enable_chroma` alongside `enable_lcs`,
  reaching full parity with the CPU reference and shipped GPU twins. When `enable_chroma`
  is set, Metal computes `float_ms_ssim_cb` and `float_ms_ssim_cr` across 3 planes on the GPU
  and advertises them in `provided_features` and `core/src/metal/dispatch_strategy.c`.
  Subsampled chroma requires at least 176x176 dimensions (351x351 luma for YUV420P
  because plane allocation uses ceil subsampling; 352x352 is the next even input),
  enforced at init; YUV400P remains luma-only even when chroma is requested.
  When `enable_db` and `clip_db` are set, scores are clipped at the
  frame geometry `max_db` ceiling (`ceil(10 * log10(peak² / mse))`). Device-free
  executable option-semantics and mutation contract tests run without a Metal
  device, while updated Apple-Silicon parity tests verify the complete path.
  Each runner now owns a fresh option dictionary, and every L/C/S atom is
  rejected before weighted combination if it is non-finite. Mutation controls
  reject both option reuse and a post-consumption double free.
