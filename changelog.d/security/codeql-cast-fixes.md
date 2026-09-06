- **Integer index operands widened to `ptrdiff_t` in convolve, moment, psnr (2026-09-04)** —
  `core/src/feature/iqa/convolve.c` (`iqa_convolve_vertical_pass`),
  `core/src/feature/moment.c` (`compute_1st_moment`, `compute_2nd_moment`) and
  `core/src/feature/psnr.c` (`compute_psnr`) now index as
  `pic[(ptrdiff_t)i * stride + j]` instead of forming the `int * int` product first.
  Numerically inert: the same element is addressed and no floating-point
  arithmetic changed. This closes no open alert — the six
  `cpp/integer-multiplication-cast-to-long` alerts CodeQL ever raised on these
  files (30, 31, 33, 706, 707, 708) are `float * float` products accumulated
  into `double`, dismissed as false positives in June 2026 and deliberately
  left as they are: widening those operands to `double` changes the
  single-rounded product and breaks the SIMD bit-exactness contract
  (ADR-0138, ADR-0179). Bit-exactness of this change verified via
  `vmaf --precision=max` JSON diff (byte-identical before/after for `psnr_y`,
  `psnr_cb`, `psnr_cr`, `float_ssim`, `float_ms_ssim`), 13/13 convolve +
  10/10 moment unit tests, and the Netflix CPU golden-data gate
  (271 passed, 12 skipped, 0 failed).
