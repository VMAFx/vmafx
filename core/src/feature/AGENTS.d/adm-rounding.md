---
paths:
  - core/src/feature/integer_adm.c
  - core/src/feature/integer_adm_kernels.h
  - core/src/feature/arm64/adm_neon.c
  - core/src/feature/adm_cm_accumulator.h
  - core/src/feature/x86/adm_avx2.c
  - core/src/feature/x86/adm_avx512.c
invariant: Integer ADM i4_adm_cm rounding overflow, row rounding, scale-0 masking, and gain limits.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Integer ADM Rounding Overflows and Masking Limits

- **`integer_adm.c` i4_adm_cm int32 rounding overflow**
  (fork-inherited, ADR-0155): both `add_bef_shift_flt[]`
  initialiser loops in
  [`integer_adm.c`](../integer_adm.c) (scales 1–3) assign
  `1u << 31 = 0x80000000` into `int32_t`, which wraps to
  `-2147483648`. rounding term is sign-negated; every
  downstream `(prod + add_bef_shift) >> 32` subtracts 2^31
  instead of adding it. **Deliberately preserved** — buggy
  arithmetic is encoded in Netflix golden
  `assertAlmostEqual` values (project hard rule #1 /
  [ADR-0024](../../../../docs/adr/0024-netflix-golden-preserved.md)).
  Do NOT widen `add_bef_shift_flt[]` to `uint32_t` or `int64_t`
  without coordinated Netflix-authored golden-number update
  ([ADR-0142](../../../../docs/adr/0142-port-netflix-18e8f1c5-vif-sigma-nsq.md)
  carve-out). Netflix upstream #955 is OPEN since 2020 with no
  maintainer response — until it closes with fix,
  overflow stays. See
  [ADR-0155](../../../../docs/adr/0155-adm-i4-rounding-deferred-netflix-955.md)
  and [rebase-notes 0048](../../../../docs/rebase-notes.md).
  CUDA mirrors name same negative value directly as `INT32_MIN` in
  `cuda/integer_adm/adm_csf.cu` and both fused paths in
  `cuda/integer_adm/adm_cm.cu`. Do not restore `1u << 31`
  unsigned-to-signed conversion there: NVCC diagnoses it as `#68-D`, while
  widening it would violate this numerical invariant.
- **`integer_adm` row-level rounding invariant** (fork-local, ADR-1167):
  In integer ADM contrast masking, scalar CPU reference, AVX2/AVX-512 CPU
  paths, and CUDA, HIP, SYCL and Metal twins all apply inner
  accumulation rounding shift:
  `(row_total + add_shift_inner_accum) >> shift_inner_accum` must NEVER be
  distributed across pixel, lane, warp, subgroup or threadgroup reduction.
  Bitwise right-shift with rounding bias is non-linear and non-distributive
  over addition. Every implementation must accumulate all columns of row in
  64-bit precision across entire width `[start_col, end_col)` before
  applying shift once per row. Scalar CPU, CUDA, HIP and SYCL (since
  ADR-1362, `adm_dev_fold_row`) use private `adm_cm_round_row_total()`
  seam; AVX2/AVX-512 hold no fold of their own: their entry points hand
  interior-row callback to scalar drivers `adm_cm_rows()` /
  `i4_adm_cm_rows()` (`integer_adm_kernels.h`), which fold once per row
  through `adm_cm_fold()`; Metal uses MSL-local twin.
  helper's rounding term stays signed: CUDA i4 passes ADR-0155's negative
  `INT32_MIN` term, which must never be cast through `uint32_t`.
  Kernel launch grids must use `gridDim.x = 1` to ensure single-block/warp
  row traversal. Furthermore, border row selection at `i == 0 && top <= 0`
  must use explicit absolute indices `{row_top, row_bot, col_l, col_r}` and
  evaluate `csf_a` at row 0 center (`i * src_stride + j`), never walking running
  pointer offsets. Score parity cannot observe one-unit placement errors after
  float conversion; preserve `test_adm_cm_row_rounding` and
  `test_adm_cm_row_rounding_contract` as raw-value and all-backend guards;
  contract rejects private row loop or fold in `adm_avx2.c` /
  `adm_avx512.c`.
  See [ADR-1167](../../../../docs/adr/1167-adm-cm-row-level-rounding.md) and
  [Research-2111](../../../../docs/research/2111-adm-cm-row-rounding-observability.md).
- **`integer_adm` scale-0 masking: int32 centre tap, int64 clamped excess**
  (fork-local, ADR-1402; departs from upstream master until Netflix/vmaf
  #1602 merges). Centre tap `((8738 * |a|) + 2048) >> 12` stays int32
  (max 69904): `adm_cm_thresh()` in `integer_adm_kernels.h`. Excess =
  `clamp(|x| - thr * 2^shift, 0, INT32_MAX)` in int64:
  `adm_cm_excess_s0()` in `adm_cm_accumulator.h`, two selects, no branch
  (branch mispredicts on noise: scalar AIM stage +90%). Every twin = scalar
  bit for bit: `cm_excess_avx2()` / `cm_excess_avx512()`, NEON
  `adm_neon_cm_excess_s0()`, CUDA + HIP call
  helper, SYCL `adm_dev_cm_excess_s0()`, Metal `adm_cm_excess_s0()` (MSL).
  Never restore: `(int16_t)` cast on tap, `srai(slli(centre, 16), 16)`
  in vector thresholds, `adm_i16()` on SYCL centre term,
  `abs(x) - (thr << shift)` anywhere (undefined for `thr < 0`). Change one
  implementation -> change all eight (NEON included), rerun golden gate. x86 edge rows + rows
  narrower than one block run shared scalar `adm_cm_row()`; leftover columns
  = top lanes of one more vector block, no scalar tail. Guards:
  `test_integer_adm_cm_threshold` (clamp pinned; patch pictures <= 1 on every
  dispatch level), `test_integer_adm_simd` (vector vs scalar kernels,
  thresholds of either sign, products past int32), `test_gpu_adm_tiny_frames`
  (patch content on CUDA / HIP / SYCL). Rebase map:
  [rebase-notes](../../../../docs/rebase-notes.md) "fix/adm-cm-centre-tap-wrap".
- **`integer_adm` enhancement gain limit = scalar's truncated double
  product** (fork-local, ADR-1413). Scalar: `rst = MIN(rst * gain, t)` /
  `MAX(...)`, double stored in integer -> `trunc(fl(rst * gain))`, then
  integer min / max (`t` integral, so order does not matter). Every twin
  returns it for any limit in [1, 100]: x86 `cvttpd` conversions, CUDA + HIP
  C cast (`cvt.rzi.s32.f64`), SYCL `adm_gain_limit_product()` from
  `adm_gain_limit.h` (two 64-bit products + bit-length comparison, no
  double; exact, not Q31). Never: rounding conversion, floor,
  fixed-point limit. Metal still multiplies in binary32
  (`T-METAL-ADM-GAIN-LIMIT-FLOAT32-2026-10-01`). Guards:
  `test_integer_adm_simd`, `test_adm_gain_limit` (host),
  `test_gpu_adm_tiny_frames` (limits 1.2, 1.5; bit-exact on SYCL).
- **`adm_decouple_neon()` = scalar decouple, any gain limit** (Netflix/vmaf
  `9e48141b`, ported; ADR-1413 applies). Vector path only for integral
  limit, where `rst * gain` is int32 product (|rst| <= 2^15, limit <= 100)
  and equals truncated double product; fractional limit or band
  narrower than four columns runs `adm_decouple_cols()` in
  `integer_adm_kernels.h`. Never give vector path fractional limit
  without `vcvtq_s64_f64` (truncating) on double product, never rounding
  conversion. Angle test = `adm_angle_flag_fp64()` in double; gain 1 skips it
  (Q15 reconstruction already lies between 0 and `t`). change to
  `adm_decouple_band()` / `adm_decouple_cols()` changes kernel in
  same PR. Guards: `test_integer_adm_simd` (aarch64: gains 1, 1.2, 1.5, 2, 3,
  7, 100, -32768 and angle-boundary samples; fails on `+1` in limited
  sample), `make test-netflix-golden-arm64`.
- **`integer_adm` scale-0 contrast-masking rows summed unsigned**
  (T-ADM-CM-SCALE0-ROW-INT64-OVERFLOW-2026-10-05; departs from upstream,
  which sums int64). Scale-0 terms = non-negative cubes; row passes
  INT64_MAX at default weights on 31-32 / 63-64 px wide pictures (1.044 /
  1.021 INT64_MAX = 0.52 of 2^64, `core/test/adm_cm_row_overflow_frame.h`) and
  with h/v CSF weight above 38,400 at 16K. Below 2^64 up to h/v
  weight of about 45,200; scale-0 h/v limit of ADR-1917 (43,900) keeps
  every row below 0.92 of 2^64 (`scripts/dev/adm_cm_row_bound.py`). So scale 0 =
  `uint64_t` row (`adm_cm_accum_px()`, `AdmCmRowFn`, `cm_row_avx2/512()`
  via `hsum_epu64`), `adm_cm_fold_s0()` + `adm_cm_round_row_total_s0()`,
  `uint64_t` frame accum into `adm_cm_result()`. Twins: CUDA
  `warp_reduce_u64()` + `uint64_cu` rows, HIP `uint64_cu` shared tree, SYCL
  `uint64_t` partials + unsigned fold (every scale, all terms non-negative),
  Metal already `ulong` rows, host sums `uint64_t`; GPU hosts reinterpret
  device int64 slots as `uint64_t` for scale 0. Scales 1-3 stay signed
  (2.8x margin; CUDA i4 negative rounding, ADR-0155). Below 2^63 bits
  are signed form's. Never restore int64 scale-0 row or signed
  scale-0 fold. Guards: `test_integer_adm_cm_row_unsigned` (64x64, 8/10/16
  bit, SIMD == scalar), `test_gpu_adm_tiny_frames`
  (`test_gpu_adm_row_past_int64_max_parity`),
  `test_adm_cm_row_rounding_contract.py`.
