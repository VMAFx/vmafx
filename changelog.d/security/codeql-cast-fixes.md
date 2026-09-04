- **CodeQL integer multiplication cast widening in convolve, moment, psnr (2026-09-04)** —
  widened floating-point operands before multiplication at six sites in
  `core/src/feature/iqa/convolve.c` (`iqa_convolve_horizontal_pass`,
  `iqa_convolve_vertical_pass`, `iqa_filter_pixel`),
  `core/src/feature/moment.c` (`compute_2nd_moment`), and
  `core/src/feature/psnr.c` (`compute_psnr`). Previously, `float` operands
  were multiplied in single-precision before implicit or explicit widening
  to `double` accumulator variables, triggering CodeQL
  `cpp/integer-multiplication-cast-to-long` / float truncation warnings.
  Casting operands to `(double)` before multiplication eliminates the truncation
  hazard. Bit-exactness verified via `vmaf --precision=max` JSON diff (byte-identical
  frame scores across `psnr_y`, `psnr_cb`, `psnr_cr`, `float_ssim`, `float_ms_ssim`),
  10/10 feature unit tests passing, and the full Netflix CPU golden-data gate
  (248 passed, 12 skipped, 0 failed). Closes CodeQL alerts 922, 923, 924, 925,
  926, 928.
