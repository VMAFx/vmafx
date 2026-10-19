---
paths:
  - core/src/feature/hip/integer_vif_hip.c
  - core/src/feature/hip/integer_vif_hip.h
  - core/src/feature/hip/integer_vif/vif_statistics.hip
invariant: vif_hip produces exact CPU bits and enforces 16x16 minimum frame dimensions.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# vif_hip = CPU bits (ADR-1435, `EXACT_TWINS`)

Scores = `integer_vif.c`'s bits (gfx1036: 440 of 440 scores, six fixtures;
`debug=true` sums too). Rebase-sensitive:

- Every per-pixel logarithm = lookup in CPU's table. Host builds it with
  `vif_log2_table_generate()` (`../vif_log2_table.h` via `integer_vif.h`,
  table's one definition; SYCL and Metal hosts call it too),
  uploads to `log2_table_dev` in `vif_hip_tables_upload()`; horizontal kernels
  take it as argument, `log2_lookup()` masks with
  `VIF_HIP_LOG2_TABLE_SIZE - 1u` like `log2_32()` / `log2_64()`.
- No `log2f()` / `__float2int_rn()` / `roundf()` in `vif_statistics.hip`.
  Device `log2f()` = one ulp off glibc on 15964 of 32768 arguments, ties
  round to even: 77 entries one lower, nearly every frame up to 5.4e-7 off.
- No second table expression in `integer_vif_hip.c` or `integer_vif.c`.
- Kernel argument order of five horizontal kernels: `...,
  vif_enhn_gain_limit, log2_table, accum_out`. Host `args_hori[]` must match.
- Guards: `test_hip_vif_parity` + `_large` (device, `==`, seven cases),
  `test_hip_vif_log2_table_contract.py` (device-free, five planted
  regressions).

## vif_hip minimum 16x16 (ADR-1381)

- `vif_hip_min_dim()` from `vif_filter1d_width`: `(half + 1) << scale` over
  scale filters {17, 9, 5, 3} and decimation filters {9, 5, 3} = 16, same as
  `vif_sycl`. `mirror2_i()` clamps, so no fault below it, but other samples
  than CPU; scale 3 empty below 8.
- `check_context_hip()` + `context_fallback_name = "vif"`: model dispatch runs
  CPU `vif` below bound (ADR-1324). `init()` refuses direct request with
  `-EINVAL` before any device work, after scaffold `-ENOSYS` (ADR-1264).
  Guard: `test_hip_vif_min_dim` (skips on scaffold builds).

- **Residual variance via `vif_sv_sq()` (ADR-1561).** Kernel defines
  `VMAF_IVIF_FUNC` as `static __device__ __forceinline__` and includes
  `feature/integer_vif_sv_sq.h`; `sv_sq` is `uint32_t`. No raw
  `double` -> `int32_t` conversion of `sigma2_sq - g * sigma12`
  (`test_integer_vif_sv_sq_contract.py`). Header listed in backend's
  `depend_files` (`core/src/meson.build`).
