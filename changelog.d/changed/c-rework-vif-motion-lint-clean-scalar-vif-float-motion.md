- The scalar VIF and float-motion extractors (`core/src/feature/integer_vif.c`,
  `core/src/feature/vif_tools.c`, `core/src/feature/float_motion.c`) are now
  lint-clean to the fork's strictest clang-tidy profile (45 findings discharged:
  14 / 26 / 5 → 0 / 0 / 0) and to cppcheck, with the Netflix headers kept.
  Twelve oversized functions were split into named helpers: the integer VIF
  statistic's horizontal pass, per-pixel log-domain accumulation and final
  reduction are single shared helpers behind `vif_statistic_8`,
  `vif_statistic_16` and the SIMD-tail helper `vif_compute_line_residuals`
  (which now takes a `const VifPublicState *`); the float VIF separable
  filters share one reflect-101 index / vertical / horizontal helper set; and
  `float_motion` keeps its Y / U / V working sets in a `MotionPlane` array with
  plane-level alloc / free / blur / score helpers, dropping its three
  `readability-function-size` NOLINTs. `round` / `ceil` / `floor` on `float`
  operands use the `f` variants (the VIF log2 LUT is proven bit-identical over
  all 32768 entries). Behaviour deltas are confined to error paths:
  `float_motion` with `motion_add_uv` on a chroma-less pixel format now rejects
  the format before allocating (the Y-plane buffers no longer leak on that
  `-EINVAL`), and the scalar float VIF filters log an error and return instead
  of dereferencing NULL when their per-row scratch allocation fails. Netflix
  golden scores, the AVX2 / AVX-512 / scalar lanes and every feature option
  path are byte-identical at `--precision max` (31-case matrix). C translation
  units keep `NULL` per ADR-1138.
