---
paths:
  - core/src/feature/x86/motion_avx2.c
  - core/src/feature/x86/motion_avx512.c
  - core/src/feature/x86/float_motion_avx2.c
  - core/src/feature/x86/float_motion_avx512.c
invariant: motion_v2_avx2.c using logical shift is knowingly out-of-spec vs scalar; do not port to NEON.
---
# Motion v2 SIMD Divergence Notes

| Group | TUs that move in lockstep |
| --- | --- |
| **Motion v2 NEON / AVX2 divergence** (ADR-0145) | `motion_v2_avx2.c` (currently uses `_mm256_srlv_epi64` *logical*) is **knowingly out-of-spec** vs scalar; `../arm64/motion_v2_neon.c` matches scalar via arithmetic shift. Do NOT port AVX2 logical pattern to NEON. AVX2 audit is separate batch. |

## Pipelines are row loops over inlined stages

`motion_avx2.c` and `motion_avx512.c` keep `motion_score_pipeline_{8,16}_*` as
row loops. vertical pass of one row is `y_conv_row_{8,16}_*` (returns whether
any `y_row` value is non-zero, which is what lets pipeline skip
horizontal pass); horizontal pass is `x_conv_row_sad_*`. Each has one vector
block helper and one scalar helper for columns vector loop cannot reach.
change to arithmetic goes into helper that holds statement, on
both files, and `core/test/test_motion_v2_simd.c` /
`core/test/test_motion_avx512_parity.c` compare result with scalar
reference. Keep every function at or under 60 lines (ADR-1142).

## `sad_avx512` takes the difference in unsigned lanes

`sad_avx512()` forms `|a - b|` as `max_epu16(a, b) - min_epu16(a, b)`.
signed 16-bit subtraction followed by `abs_epi16` wraps for 16-bit samples
that differ by more than 32767 (65535 against 0 gave 1). 16-bit cases of
`test_sad_avx512_*` in `core/test/test_motion_avx512_parity.c` fail on that
form (T-SIMD-SAD-AVX512-INT16-DIFFERENCE-2026-10-05).
