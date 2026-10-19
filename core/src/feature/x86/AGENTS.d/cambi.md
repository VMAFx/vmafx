---
paths:
  - core/src/feature/x86/cambi_avx2.c
  - core/src/feature/x86/cambi_avx512.c
  - core/src/feature/cambi.c
  - core/src/feature/cambi.h
  - core/src/feature/cambi_c_values_frame.h
invariant: Dispatch binds calculate_c_values_scan_avx2, not upstream calculate_c_values_avx2.
---
# CAMBI Stage Kernels and Scanned C-Values Invariants

| Group | TUs that move in lockstep |
| --- | --- |
| **CAMBI stage kernels** (ADR-1256, Research-2065) | `cambi_avx2.c` (upstream mirror + fork-local scanned c-values driver at end) + `cambi_avx512.c` + `../arm64/cambi_neon.c` (fork-local) + scalar `../cambi.c`; AVX2 / AVX-512 / NEON c-values drivers share walk `../cambi_c_values_frame.h`. New AVX2 stage → AVX-512 + NEON twin same PR; dispatch only if measured faster. Tests: `test_cambi_stage_simd.c` (every stage kernel vs shipped scalar, guard bands), `test_cambi_dispatch_invariance.c` (whole extractor per cpumask), `test_cambi_simd.c` (c-values row). |
| **CAMBI spatial-mask rows** (ADR-1256) | `cambi_avx2.c` (`compute_dp_row_avx2`, `compute_mask_row_avx2`) + `cambi_avx512.c` (`compute_dp_row_avx512`, `compute_mask_row_avx512`) + `../arm64/cambi_neon.c` (`compute_dp_row_neon`, `compute_mask_row_neon`) + scalar reference in `../cambi.c` (declared in `../cambi.h`). Adapted from upstream `86da14d03` but **not verbatim — keep fork's versions on sync**: dp row keeps only `carry += broadcast(block total)` on loop-carried chain (upstream's form is slower than scalar under Clang / icx); AVX2 mask row biases both compare operands by 2^31 so signed `vpcmpgtd` equals scalar unsigned compare for every input. Dispatch only what beats scalar on measured run (ADR-1256); re-bench before wiring changed kernel. Tested in `../../test/test_cambi_spatial_mask_simd.c`. |

## CAMBI AVX2 scanned c-values invariants (Research-2065)

- Dispatch binds `calculate_c_values_scan_avx2`, not upstream
  `calculate_c_values_avx2` (0.81x scalar under icx). Upstream driver stays
  built + tested; do not delete, do not re-bind without re-measuring.
- Scans `scan_row_avx2` / `scan_slide_avx2` out of line
  (`CAMBI_SCAN_NOINLINE_AVX2`), constants per call: no ymm live across
  row-kernel calls (ADR-1254).
- Band test unsigned via `min_epu16(v - base, size - 1) == v - base` (AVX2 has
  no unsigned 16-bit compare). `size >= 1` guaranteed by `alloc_cambi_buffers`.
- No masked loads on AVX2 → scalar tail (`cambi_column_*`) < 16 cols. Never
  vector-load past last column; bit set past `n` → helper writes outside
  histogram.
- `packs(lo128, hi128)` + `movemask_epi8` = one bit per column, in order.

## CAMBI AVX-512 invariants (Research-2065)

- Scans `scan_row_avx512` / `scan_slide_avx512` stay out of line
  (`CAMBI_SCAN_NOINLINE`). Inlined → Clang hoists band broadcasts, spills zmm
  around each row-kernel call → Win64 fault risk (ADR-1254). One call per
  256-column block keeps call cost low.
- Scan may over-flag, never under-flag. Mirrors `uh_slide` skip + band test in
  `../cambi.h`; change one → change both.
- Masked row tails (derivative, decimate, anti-dither, mode filter) load-bearing
  for speed: unmasked derivative tail (≤ 32 scalar cols) lost to AVX2.
- `calculate_c_values_row_avx512` keeps scalar tail: 4-byte gather on last lane
  reads 2 B past histogram end.
- Range updater width irrelevant (512 vs 256 within ±3 %). Gain = column scan,
  not width.
