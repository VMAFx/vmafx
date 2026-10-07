---
paths:
  - core/src/feature/hip/integer_adm_hip.c
  - core/src/feature/hip/integer_adm_hip.h
  - core/src/feature/hip/integer_adm/adm_cm.hip
  - core/src/feature/hip/integer_adm/adm_csf.hip
  - core/src/feature/hip/integer_adm/adm_dwt2.hip
  - core/src/feature/hip/integer_adm/adm_decouple_inline.hip
invariant: Integer ADM maintains exact CPU parity, staging buffer rules, int64 vertical sums, and single reflection clamping.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# adm_hip = CPU bits (ADR-1423, `EXACT_TWINS`)

Scores = `integer_adm.c`'s bits (gfx1036: 21 pairs, 6192 values with
`debug=true`). Rebase-sensitive:

- Host arithmetic = CPU routines of `integer_adm_kernels.h`, never copy:
  `adm_csf_factors()` (weights), `adm_csf_den_ctx_init()` /
  `i4_adm_csf_den_ctx_init()` (border + every denominator shift, passed to
  kernel), `adm_cm_ctx_init()` / `i4_adm_cm_ctx_init()` on empty
  `AdmBuffer` + `adm_cm_result()` / `i4_adm_cm_result()` /
  `adm_csf_den_result()` / `i4_adm_csf_den_result()` (scores). No
  `dwt_quant_step()`, `adm_csf_factors()`, `conclude_adm_*()` definition in
  `integer_adm_hip.c`.
- `adm_skip_scale0`: numerator 0, denominator `(float)1e-10`, BOTH added to
  frame sums like `integer_adm_scale0()`.
- `adm_csf_den.hip`: one block per row + band (`grid = 1 x rows x 3`, 128
  threads), thread sums in shared memory, ONE fold per row by thread 0
  through `adm_csf_den_round_row_total()` (`adm_cm_accumulator.h`, also
  CPU's fold, ADR-1416).
  Never fold per thread / wave / block-of-columns: accumulator differs, score
  differs on low-detail frames (4e-7; 1.5e-5 on test's sparse frame) and
  at scale 0 above 2^20 region samples.
- No logarithm in that file: fp32 `log2f(area) - 20` is off by one for 81
  areas right above power of two (962x13542: `adm_scale0` 0.860 vs 0.979).
- Per frame: `adm_hip_stage_luma()` (upload) FIRST, then
  `hipMemsetAsync(buf->tmp_res)`, then kernels. Clear queued ahead of
  upload = lost in first context needing larger planes than earlier
  contexts of process (recycled device memory -> NaN numerator, run
  fails). `hipMemset` at allocation = no fix: async on null stream for
  device memory (hipamd `ihipMemset()`, ROCm 7.2). Rule for every twin:
  "Frame order: upload, clear, kernels" above (ADR-1427).
- Guards: `test_hip_adm_exact` (device, nine contexts in one process, two
  frames each, `==`), `test_hip_adm_exact_contract.py` (device-free, ten
  planted regressions).

## Integer ADM staging buffer requirement (ADR-1154, ADR-1211)

HIP pictures arrive with host pointers (host-pic backend, ADR-0530);
host pointer handed to device kernel faults GPU
(T-HIP-INTEGER-ADM-GPU-PAGE-FAULT-2026-09-05). `integer_adm_hip.c`
reads device copy of scale-0 luma plane per side (ADR-1211,
PR #1370), since ADR-1408 context's shared frame
(`adm_hip_stage_luma()`); rows are packed, so kernel stride is `w`.
ADR-1154 deferral is over: do not re-add `should_fail` to HIP ADM
tests for it. Float ADM (`float_adm_hip.c`) has its own staging.

## AIM pass and dispatch (ADR-1525)

- AIM kernels: `adm_cm_aim_line_kernel_4` (scale 0) and
  `i4_adm_cm_aim_line_kernel` (scales 1-3) in `adm_cm.hip`, CUDA
  ADR-0746 kernels ported. Signal csf(a); threshold 3x3 |csf(r)| / 30,
  centre |csf(r)| / 15, recomputed from bands. change to
  `adm_cm_ctx_init()` / `i4_adm_cm_ctx_init()` with `measure_aim` or to
  `adm_csf_cols()` / `i4_adm_csf_cols()` in `integer_adm_kernels.h` changes
  these kernels in same PR.
- Every AIM shift comes from CPU's context on host
  (`adm_cm_s0_launch()`, `i4_adm_cm_aim_device_hip()`), scale-0 DLM
  launch included; no AIM kernel derives shift with `log2f`
  (`test_hip_adm_exact_contract`).
- AIM accumulators = third block of result buffer
  (`RES_BUFFER_SIZE = RES_SLOTS_PER_TERM * 3`), cleared and read back with
  frame. Host concludes each AIM scale with `adm_cm_result()` /
  `i4_adm_cm_result()` at noise weight 0, skips scale 0 under
  `adm_skip_scale0`, reports 0 under `adm_skip_aim`.
- `.flags = VMAF_FEATURE_EXTRACTOR_HIP` and `aim` / `adm3` claim go
  together; never flag twin that cannot emit every feature default
  model reads from it.
- Every signed right shift of `adm_cm.hip` goes through `adm_asr()` (sign
  fill on unsigned bits); bare `>>` on signed value is
  `bugprone-signed-bitwise` finding and file is at 0.

## Integer ADM tiny frames (T-GPU-ADM-TINY-FRAME-SHIFT-2026-09-18)

- `init_fex_hip()` calls `adm_frame_size_check()` first, before any device
  resource. Bound = CPU bound (17x17).
- Host shift rounding constant = `adm_half_shift(x)`. In-kernel scale-0 shift
  in `adm_cm_reduce_line_kernel_body` -> guarded ternary, 0 when shift = 0.
  Never bare `1u << (x - 1)`.
- Scale-0 CM kernel (`adm_cm_line_kernel_body`): `x + 1` -> `min(.., w - 1)`,
  `y + 1` -> `min(.., h - 1)`; `x - 1`, `y - 1` -> `abs()` (ADR-1210 rule).
- HIP twin emits `adm3_score` / `aim_score` since ADR-1525; shared CUDA/HIP
  tests request `adm3` in both arms.
- HIP ADM tests run without `should_fail` since ADR-1211 staging; all pass on
  gfx1036. Do not re-add `should_fail` to hide failure.

## Integer ADM 16-bit vertical DWT sums in int64 (T-GPU-ADM-DWT2-16BIT-INT32-OVERFLOW-2026-09-18)

- `core/src/feature/hip/integer_adm/adm_dwt2.hip`, scale-0 fused kernel: vertical accumulator = `DwtVertAccum<T>::type`
  -> int64 for `uint16_t`, int32 for `uint8_t`.
- Low-pass taps 1-3 sum 50582 -> int32 sum overflows (UB) once 3 16-bit
  samples >= 42456. CPU twin: `adm_dwt2_vpass16_tap4()` (int64).
- Normalised value fits int32 -> int64 form = old wrapped result. Scores
  identical; never narrow back to int32 for speed.
- Guard: `test_gpu_adm_bright_16bit_parity` in `test_gpu_adm_tiny_frames.c`
  (parity only; device wrap hides UB itself).

## Tile loads and ADM scale-0 rows clamp after one reflection (ADR-1381)

- Tiled kernels load whole tile for every thread, padding threads included.
  One reflect-101 keeps every consumed sample in plane, not padding samples:
  17-sample motion plane reflects halo 33 to -1.
- Motion tile loads: `vmaf_hip_tile_index(vmaf_hip_reflect_101(i, n), n)`
  (`hip_tile_index.h`). Identity for every consumed sample -> no score change.
  `float_motion_score.hip` same geometry, same clamp (`fm_tile_index()`); its
  old `fm_mirror()` read before `ref_in` at extents 3-9 and 17.
- ADM scale-0 vertical DWT: `adm_dwt2_load_column()` reads
  `adm_dwt2_source_row()` (`integer_adm/adm_dwt2_rows.h`); launch geometry
  `ADM_DWT2_*` shared by kernel (`static_assert`) and `integer_adm_hip.c`.
  Bare reflection escapes only for heights 1-8; ADM minimum 17 -> identity.
  Scale 1-3 vertical kernels read per output row, in bounds from 2 rows.
- `test_hip_adm_dwt2_rows`: host replay of every launched thread row, heights
  1-8192, and every motion tile slot, extents 3-1024. Device-free, fast suite.
- New tiled HIP kernel: route every halo load through `vmaf_hip_tile_index()`.
- **Scale-0 decouple reciprocal = `div_lookup`, by arithmetic
  (`T-GPU-ADM-DECOUPLE-FP32-RECIPROCAL-2026-10-03`).** `adm_recip_q30()` in
  `integer_adm/adm_decouple_inline.hip`: fp32 quotient `q` (off by at most 11),
  exact int32 remainder, `floor(r / |o|)` added with sign of `o`. Equal to
  `div_lookup[o + 32768]` for every `int16` operand; `int32_t(2^30f / float(o))`
  alone is another integer for 343 positive operands and moves restored sample
  above |o| = 16566. Do not restore it, and do not write 32-bit integer division
  there: `adm_cm_line_kernel_8` goes 148 to 228 registers and
  `test_cuda_adm_cm_register_pressure` (ADR-1226) fails. Guards:
  `test_adm_decouple_recip_hip` (this header compiled for host against
  `adm_decouple_band()`, every `int16` operand, no device) and case
  `test_adm_attenuated_detail_exact` of `test_hip_adm_exact` on device.
- **Scale 1-3 decouple bounds gain product before narrowing**
  (T-GPU-ADM-S123-GAIN-PRODUCT-NARROWING-2026-10-05). `decouple_r_s123()`:
  `gained = (double)rst_q * adm_enhn_gain_limit`, then
  `(int32_t)(rst_f > 0 ? fmin(gained, t) : fmax(gained, t))`, as CPU's
  `adm_decouple_band_s123()`. |o| reaches 1.45e9 at scale 1, so
  `(int32_t)(...) * gain` narrowed first is undefined conversion (the
  saturating device cvt hid it). Guard: `test_gpu_adm_gain_product_contract.py`.

- **`decouple_angle_flag_s0()` sums in 64 bits (`T-GPU-ADM-ANGLE-FLAG-S0-INT32-CORNER-2026-10-06`, [ADR-2134](../../../../../docs/adr/2134-cuda-adm-cm-aim-register-budget-angle-flag.md)).**
  `integer_adm/adm_decouple_inline.hip` forms dot product as unsigned sum restored by
  `(int64_t)(int32_t)(sum - 1u) + 1` (one value int32 cannot hold is 2^31, every band at -32768);
  squared magnitudes as unsigned sums widened to int64, as CPU's int64 `adm_angle_flag()` sums are. Do not
  narrow it back to int32. form costs `adm_cm_aim_line_kernel_4` 209 registers (plain int64: 216), which
  is that kernel's own budget in `test_cuda_adm_cm_register_pressure` (every other kernel 208, zero spill);
  RC7 row wins register back. `test_adm_decouple_recip_hip` holds flag to CPU's at every int16
  corner and expects 0 mismatches. It also holds `decouple_r_s123()`, `get_best15_from32()` and scale 1-3 flag to CPU's,
  so no function of header is unused in host build (CodeQL `cpp/unused-static-function`).
