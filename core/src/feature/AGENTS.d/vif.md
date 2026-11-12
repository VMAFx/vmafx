---
paths:
  - core/src/feature/integer_vif.c
  - core/src/feature/vif_tools.c
invariant: Integer VIF scalar reference tails, log2 table generation, score append order, and NULL preservation.
area: vif-motion-psnr
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer VIF Scalar Reference and Log2 Tables

## Scalar VIF statistic and float-motion plane helpers (2026-09-02, c-rework-vif-motion)

- **`integer_vif.c` = single scalar reference SIMD tails run against.**
  `vif_statistic_8`, `vif_statistic_16` and `vif_compute_line_residuals`
  (called by `x86/vif_avx2.c`, `x86/vif_avx512.c` and `arm64/vif_neon.c` for
  columns their 16-wide blocks do not cover) all go through
  `vif_horizontal_pixel` → `vif_accumulate_pixel` → `vif_store_residuals`.
  Those helpers hold upstream arithmetic verbatim (operand types and
  evaluation order included) and are `FORCE_INLINE`. Change statistic
  there once and mirror it in three kernels. Never re-inline private
  copy into one entry point, or SIMD block path and scalar tail
  diverge for widths that are not multiples of 16. Bit-exactness relies on
  fork's `-std=c23` (contraction off) and no `-march` (no FMA) flags; if
  either changes, re-run 31-case `--precision max` matrix in
  [`docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md`](../../../../docs/research/2026-09-02-c-rework-vif-motion-bit-exact.md).
- **Residual variance = `vif_sv_sq()` (ADR-1561).** `integer_vif_sv_sq.h`
  holds upstream's `int32_t sv_sq = sigma2_sq - g * sigma12; sv_sq =
  (uint32_t)(MAX(sv_sq, 0));` as one defined helper: difference truncated
  when in (0, 2^31), else 0 = x86's value (upstream conversion UB below
  INT32_MIN; aarch64 vectorised loop keeps low 32 bits). Callers:
  `vif_accumulate_pixel()`, `x86/vif_avx2.c::vif_num_log256()`,
  `arm64/vif_neon.c::vif_num_log()`, CUDA `vif_statistics.cuh`, HIP
  `vif_statistics.hip`, `test_sycl_integer_vif_math.c::reference_terms()`.
  Never re-inline the raw conversion; `sv_sq` stays `uint32_t` (unsigned
  `sv_sq + sigma_nsq`). Guards: `test_integer_vif_sv_sq` (fails under clang
  `-fsanitize=undefined` without it), `test_integer_vif_sv_sq_contract.py`.
- **`vif_log2_table_generate()` (`vif_log2_table.h`, included by
  `integer_vif.h`; plain C valid as C++ / Objective-C++) uses `roundf`**, proven
  bit-identical to upstream's `round` over all `VIF_LOG2_TABLE_SIZE` entries.
  same LUT feeds AVX-512 gather path (ADR-0500); do not switch rounding modes.
  One definition of the table (was `log_generate()` in `integer_vif.c`):
  the `vif_cuda`, `vif_hip`, `vif_sycl` and `vif_metal` hosts upload its
  values (ADR-1435, ADR-1462); no host keeps a copy of the expression or of
  `VIF_LOG2_TABLE_SIZE` (`test_hip_vif_log2_table_contract.py`,
  `test_cuda_vif_log2_contract.py`). Upstream change to table expression ->
  change there, nowhere else. No twin computes table on device.
- **`write_scores` append order is output contract**: four scale scores,
  then `integer_vif` / `_num` / `_den`, then num / den per scale 0..3.
  `double` totals are explicit left-to-right sums — keep them out of loops.
- **C translation units keep `NULL`** (ADR-1138): `integer_vif.c` and
  `float_motion.c` carry file-scoped
  `NOLINTBEGIN/END(modernize-use-nullptr)` bracket; keep `NOLINTEND` at
  end of file when appending. `vif_tools.c` has no null-pointer constants
  (`grep -c NULL core/src/feature/vif_tools.c` and `grep -c NOLINT` on
  same file both print `0`) and therefore carries no bracket — do not add
  one unless upstream hunk brings `NULL` into that file. `flush()` in
  `float_motion.c` carries cited `cppcheck-suppress constParameterCallback`
  because `VmafFeatureExtractor.flush` callback type fixes its prototype.

- **VIF log2 LUT invariant** (ADR-0500): `VifPublicState.log2_table` is
  32768-entry table (64 KB, not 65537 entries / 128 KB). normalisation in
  `log2_32` / `log2_64` always produces indices in `[32768..65535]`; mask
  `& (VIF_LOG2_TABLE_SIZE - 1u)` strips bit 15 to get `[0..32767]` index.
  If upstream Netflix changes LUT size or normalisation logic, audit
  mask in `integer_vif.h` and three gather sites in `vif_avx512.c`
  before merging. `vif_log2_table_generate()` fills `log2_table[i] = log2f(32768+i)*2048`;
  original filled `log2_table[i] = log2f(i)*2048` for `i` in `[32767..65535]`.
- **compute_vif filter-cache parameter** (ADR-0500): `compute_vif` in `vif.c`
  accepts two nullable trailing parameters `precomputed_filters` /
  `precomputed_filter_widths`. `float_vif.c` caller passes pre-computed
  Gaussian coefficients from `VifState.filter_cache` / `filter_width_cache`
  (populated once in `init()`). internal `vifdiff` path passes NULL to
  retain original per-call `vif_get_filter()` path. If upstream changes
  `compute_vif`'s signature, both declaration in `vif.h` and internal
  call in `vif.c` need updating.
