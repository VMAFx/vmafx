---
paths:
  - core/src/feature/x86/adm_avx2.c
  - core/src/feature/x86/adm_avx512.c
  - core/src/feature/integer_adm.c
  - core/src/feature/adm_csf_fixed_point.h
invariant: Integer ADM tail bounds are measured from loop start, not from column 0.
---
# Integer ADM SIMD Invariants and Declaration Cleanup

| Group | TUs that move in lockstep |
| --- | --- |
| **Integer ADM p-norm callback ABI** (ADR-0645) | `adm_avx2.c` + `adm_avx512.c` + scalar `../integer_adm.c` + headers `adm_avx2.h` / `adm_avx512.h`. `adm_cm` and `i4_adm_cm` signatures must carry `adm_p_norm` through every twin so `integer_adm:adm_p_norm=...` is not silently ignored by x86 SIMD dispatch. Default `3.0` expression shape remains Netflix-compatible path. |

| Group | TUs that move in lockstep |
| --- | --- |
| **ADM decouple LUT prefetch** (ADR-0502) | `adm_avx512.c` (`decouple_prefetch_avx512`, called by `adm_decouple_avx512`). 16-iteration software-prefetch block before `vpgatherdd` cluster must stay at distance 2 iterations (j+32). If arithmetic body between prefetch and gather shrinks below ~100 cycles, increase to 3 iterations (j+48); if it grows above ~500 cycles, decrease to 1 (j+16). prefetch target is `_MM_HINT_T1` (L2) not `_MM_HINT_T0` (L1) — immediately following band-buffer loads would evict L1 lines before gather executes. **Do not convert to `_MM_HINT_T0`; do not inline into `_mm512_prefetch_i32gather_ps` — latter requires `<zmmintrin.h>` and is not portable across all AVX-512 toolchains.** |

## Integer ADM declaration cleanup (2026-09-08)

Keep AVX2 and AVX-512 CM/CSF band descriptors and first-row threshold
aliases read-only locally. Pointed-to output buffers remain writable;
this does not change `AdmBuffer *` dispatch ABI. Scalar-tail temporaries
and row accumulators belong to their existing inner scopes. Preserve every
integer shift, float/double promotion, reduction order and prefetch distance;
const qualification is not permission to change numerical expressions.
Same-ISA old/new validation described in
[cleanup digest](../../../../../docs/research/2042-adm-simd-native-lint-2026-09-08.md).

- **No x86-64-only intrinsics.** `_mm_extract_epi64`,
  `_mm256_extract_epi64`, `_mm_cvtsi128_si64` and like go through
  `extract_epi64_128()` / `extract_epi64()` (ADM) or store
  (`_mm_storel_epi64`). fork is 64-bit only (ADR-1258), but these files
  stay 32-bit clean because they are upstream-mirror code and that is
  Netflix#1481's fix (T-X86-64-ONLY-INTRINSICS-2026-09-18).
- **Integer ADM tail bounds are measured from loop start, not from
  column 0.** `adm_decouple_avx2` uses `right - ((right - left) % 8)`, like
  `adm_decouple_s123_avx2` and every AVX-512 decouple (Netflix/vmaf
  `03b5562c5`). DWT2 kernels use
  `half_w >= 2 ? half_w - 1 - ((half_w - 2) % N) : 1` (fork `0ed57f9f1`),
  which is not upstream's `(half_w - 2) - ((half_w - 3) % N)`. Keep fork
  form on sync. `test_integer_adm_simd` and `test_adm_dwt2_x86` fill
  outputs with guard pattern at production band stride and fail on any
  store outside region
  ([Research-2063](../../../../../docs/research/2063-upstream-sync-2026-09-adm-vif-simd.md)).
- **Integer ADM shift rounding = `adm_half_shift(x)`, never
  `(uint32_t)pow(2, (x - 1))`.** Frame width 17..32 -> scale-0 cube shift 0
  -> upstream form converts inf. AVX-512 build: `vcvttsd2usi` ->
  `0xFFFFFFFF`, scale 0 off by up to 0.01. Helper lives in
  `../adm_csf_fixed_point.h`, shared with scalar. Keep on sync.
  `test_integer_adm_tiny_frames` sweeps w 17..32 vs scalar; UBSan lane
  flags old form (T-ADM-AVX512-SMALL-WIDTH-SCALE0-2026-09-18).
- **Integer ADM scale-0 CM: int32 centre tap, exact excess (ADR-1402).**
  `cm_thresh_band_*()`: tap stays int32 after `>> 12`, no
  `srai(slli(tap, 16), 16)` (scalar `adm_cm_thresh()` has no `(int16_t)`
  cast). `cm_excess_*()`: short form `max(|x| - (thr << s), 0)` exact only
  for thr in [0, 2^(31 - s)); `cm_row_*()` ORs row's thresholds and sums
  row again with exact form (thr clamped to +/-2^(31 - s), signed
  max for thr >= 0, unsigned min with INT32_MAX for thr < 0) when
  `CmFrameConsts.rare` bit is set. Decoded pictures never take pass two.
  AVX2 cube shift = arithmetic via bias: `add_cub` carries 2^63, row total
  minus `lanes * cub_bias`, every lane of every block counted (idle lanes
  too). Tail = `cm_tail_block_*()`, overlapped, lane-masked. Upstream's
  `threshold_overflow` vector form != scalar for thr < 0: do not port.
  Guards: `test_integer_adm_simd` (16 planted defects fail it),
  `test_integer_adm_simd_noise`, `test_integer_adm_cm_threshold`.
- **Integer ADM decouple gain limit: truncate, never round (ADR-1413).**
  Scalar stores `MIN(rst * gain, t)` / `MAX(...)` (double) in integer ->
  truncation toward zero. `decouple_gain_avx2()` / `decouple_gain_avx512()`:
  `_mm256_cvttpd_epi32` / `_mm512_cvttpd_epi32`; AVX-512 scale 1-3
  (`decouple_s123_limit_half_avx512()`): `_mm512_cvttpd_epi64`; AVX2 scale
  1-3 casts in C. `cvtpd` (round to nearest) = one off at limit 1.2 in every
  limited sample with product fraction >= 1/2; limits 1 and 100 hide it.
  Scale-0 angle mask: `madd_epi16` sum wraps only as 2^31 -> `INT32_MIN`;
  squared magnitudes read unsigned (`uint32_t` lanes / `cvtepu32_ps`), dot
  product replaced where it is `INT32_MIN`. Guard:
  `test_adm_decouple_matches_scalar_for_gains` in `test_integer_adm_simd`
  (limits 1, 1.2, 1.5, 100; 7 planted defects fail it).
- **Vector stores may alias anything.** `_mm*_storeu_si*` -> compiler reloads
  pointers / tables read through `buf`, `ind_x`, `ind_y`, `dst` for every
  block. Read them once per row or frame (`Dwt2Rows8`, `DecoupleBands`,
  `CmFrameConsts`, `csf_row_*()` locals). Measured: AVX-512 `adm_dwt2_8`
  +10% instructions without it.
