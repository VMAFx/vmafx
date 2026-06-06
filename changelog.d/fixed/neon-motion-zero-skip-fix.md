### Fixed

- **NEON motion pipeline false-zero early exit** (`core/src/feature/arm64/motion_v2_neon.c`):
  `motion_score_pipeline_8_neon` and `motion_score_pipeline_16_neon` used `vaddvq_s32`
  (signed horizontal sum) to check whether the phase-1 y-convolution row was all-zero.
  On checkerboard and other highly alternating inputs, positive and negative lane values
  cancelled each other to produce a sum of zero even when individual lanes were non-zero,
  causing the `x_conv_row_sad_neon` phase to be skipped for those rows.
  The result was `motion_score = 0.0` on macOS ARM64 for checkerboard test fixtures,
  and incorrectly low motion scores for other inputs.
  Fixed by replacing `neon_hadd_s32` with `neon_any_nonzero_s32`, which OR-folds the
  reinterpreted uint64 lanes so that any set bit in any lane produces a non-zero result.
