- **Docs / AI helper: strip residual `float_ansnr` references after the
  ADR-0709 / PR #38 removal.** The `float_ansnr` feature extractor was
  dropped from every backend a week ago, but seven downstream surfaces
  still described it as live: `ai/data/feature_extractor.py` mapped
  `float_ansnr` / `float_anpsnr` to a no-longer-registered extractor name
  (so any `features=["float_ansnr"]` call raised at the CLI),
  `docs/metrics/ansnr.md` was a full how-to-use page,
  `docs/backends/index.md` advertised HIP "8/11 real kernels" including
  `float_ansnr`, `docs/backends/cuda/overview.md` listed an ANSNR CUDA
  twin under SSIM / MS-SSIM / PSNR-HVS, and `docs/backends/hip/overview.md`
  listed `float_ansnr_hip` in its kernel inventory, source layout, kernel
  notes, and CLI examples. The Python helper now drops both mapping
  entries (with an inline ADR citation); the metric page is rewritten as a
  removal notice with a migration table; the HIP tally is restated as
  7/10; the CUDA and HIP overviews drop their live ANSNR references and
  keep ADR-0266 as a historical pointer. Files touched by the in-flight
  PR #295 (`docs/index.md`, `docs/metrics/features.md`,
  `docs/api/index.md`, `docs/development/build-flags.md`) are excluded
  from this PR to avoid a merge conflict; PR #295 covers the
  catalogue / build-flag / API-header side of the cleanup.
