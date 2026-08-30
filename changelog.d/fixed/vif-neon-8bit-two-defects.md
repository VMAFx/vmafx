- **`vif_statistic_8_neon` dropped up to 7 pixel columns from every row.** Its
  horizontal loop is `for (; j < uiw7; j += 8)` with `uiw7 = w > 7 ? w - 7 : 0`,
  and the row loop closed immediately after it — no scalar tail. The dispatcher
  in `integer_vif.c` installs the kernel on `flags & VMAF_ARM_CPU_FLAG_NEON`
  with **no width guard at all**, so every width was admitted and the last
  `w % 8` columns never contributed to `num`/`den`; for `w <= 7` the entire row
  was dropped. The 16-bit NEON twin, `vif_statistic_8_avx2` and
  `vif_statistic_8_avx512` all close this gap with `vif_compute_line_residuals()`
  — the 8-bit NEON kernel was the only one of the four that did not.
  **This moved the score.** On the Netflix golden pair cropped to 348×216
  (348 % 8 = 4, natural content), aarch64 gave VMAF `78.777730` against the
  scalar's `78.778275`; post-fix it matches the scalar exactly. VIF is a core
  VMAF feature, so any ARM user scoring content whose width is not a multiple of
  8 was affected.
- **`vif_statistic_8_neon` also omitted the `sigma2_sq = MAX(sigma2_sq, 0)`
  clamp**, which is width-independent and fires at `w % 8 == 0` too. The scalar
  clamps before branching, and the non-log arm then accumulates `sigma2_sq`
  into `num`; the NEON epilogue stored the raw subtraction, so a negative
  `sigma2_sq` — routine on near-flat content, where fixed-point rounding of
  `mu2` outruns the filtered `dis²` — was summed verbatim and drove `num` the
  wrong way. Symptom: `integer_vif_scale0` of **1.0000087** on a flat-step clip,
  which is impossible for a VIF ratio. The 16-bit NEON twin (`vmaxq_s32`) and
  the AVX2 kernel (`_mm256_max_epi32`) both already clamp.
- The **Netflix golden fixtures are unaffected**: 576, 1280 and 1920 are all
  multiples of 8, so the tail defect cannot fire there, and the golden pair
  scores `76.667831` pre-fix, post-fix and on the scalar path alike.
