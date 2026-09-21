- HISS-21 burn-down, `core/src/feature/` top level: the cleanup `goto`
  ladders are gone from `ciede`, `feature_collector`, `feature_dists`,
  `feature_lpips`, `float_moment`, `float_ms_ssim`, `float_psnr`,
  `float_ssim`, `motion` and `pu21` (29 HISS-01 findings), and the
  `float_ms_ssim` extractor's optional per-scale luminance / contrast /
  structure publication moved into its own helper so `extract` fits the
  60-LOC bound (1 HISS-04 finding). Scores do not move: the change is
  structural. Each former label ladder became one unwind helper that
  releases the same resources in the same order from every error exit,
  including the shallow early-error paths, and every arithmetic
  expression was moved whole rather than split across a helper boundary,
  so the compiler sees the same contraction opportunities it saw inline
  (ADR-1253). Verified with the full `meson test` suite and the Netflix
  CPU golden-data gate, assertions untouched.
