- **Metal `float_ms_ssim` CPU-aligned option semantics and score paths (T-GAP-METAL-MS-SSIM-DB-CHROMA-OPTIONS-2026-09-07 / ADR-1334).**
  The Metal `float_ms_ssim` feature extractor (`core/src/feature/metal/float_ms_ssim_metal.mm`)
  now exposes `enable_db`, `clip_db`, and `enable_chroma` alongside `enable_lcs`,
  matching the CPU reference's option surface and semantics. Metal implements
  three-plane GPU paths for `float_ms_ssim_cb` and `float_ms_ssim_cr` when
  `enable_chroma` is set and advertises them in `provided_features` and
  `core/src/metal/dispatch_strategy.c`.
  Subsampled chroma requires at least 176x176 dimensions (351x351 luma for YUV420P
  because plane allocation uses ceil subsampling; 352x352 is the next even input),
  enforced at init; YUV400P remains luma-only even when chroma is requested.
  When `enable_db` and `clip_db` are set, scores are clipped at the
  frame geometry `max_db` ceiling (`ceil(10 * log10(peak² / mse))`). Device-free
  executable option-semantics and mutation contracts verify the host logic
  without a Metal device. Updated Apple-Silicon parity cases target the
  complete runtime path, but execution on Apple hardware remains pending.
  Each runner now owns a fresh option dictionary, and each L/C/S name is bound
  to its corresponding mean and rejected before weighted combination if it is
  non-finite. Mutation controls reject every individual atom substitution or
  removal, option reuse, and a post-consumption double free.
