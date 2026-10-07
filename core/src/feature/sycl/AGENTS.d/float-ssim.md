---
paths:
  - core/src/feature/sycl/integer_ssim_sycl.cpp
  - core/src/feature/sycl/sycl_ssim_terms.h
  - core/test/test_sycl_ssim_parity.c
invariant: float_ssim_sycl terms = CPU doubles, frame sums = CPU raster order; decimation is bit-exact with CPU.
---
<!-- markdownlint-disable MD013 MD060 -->
# Float SSIM terms, sums, and decimation

- **`float_ssim_sycl` terms = CPU doubles, frame sums = CPU raster order
  ([ADR-1463](../../../../../docs/adr/1463-sycl-float-ssim-raster-sum.md)).**
  CPU (`iqa/ssim_accumulate_lane.h`): `lv = (2.0 * rm * cm + C1) / l_den`,
  `cv = (2.0 * srsc + C2) / c_den` in fp64, `sv` fp32 quotient; adds
  `lv * cv * sv`, lv, cv, sv into ONE double each, raster order; mean =
  `(float)(sum / windows)`. Twin: `ssim_float_parts()` (fp32 part) ->
  `ssim_double_terms()` = CPU's operations one for one on `SoftSigned`
  (`sycl_soft_signed.h`): exact product of two converted means, exact
  doubling, ONE rounded sum, ONE rounded quotient each. NO reduction on
  device: default kernel stores `ssim_product_bits()` = bits of
  `(lv * cv) * sv` at `row * w_final + col` (8 B / window, host
  `frame_sum_of_terms()`, shared with `integer_ssim_sycl`); `enable_lcs`
  kernel stores lv bits at `terms[i]`, cv bits at `terms[windows + i]`, fp32
  sv at `structure[i]` (20 B / window, host `ssim_frame_sums()` forms
  product and four sums, `accumulate_window()` = reference's
  statements). Kernel shape `VmafSyclKernelShape<16, 256>`, window function
  flattened, as `ssim` twin's term kernel (there SIMD-32 and default
  register file measured as scratch, ADR-1443; this shape audited
  scratch-free on A380, other shapes not measured here).
  Why not old exact integer sum (`term_fixed` + `reduce_over_group` +
  `FixedSum`): exact != CPU's running double; mean off by one float step
  on frames whose terms cancel (constructed 64x64 pair: CPU `0xb4e2b622`,
  old twin `0xb4e2b621`). Never: reduction in float twin, pair terms
  for stored term, `lv * (cv * sv)`, host sum in another order or in
  parallel chunks. On rebase: change to `ssim_accumulate_lane.h` or
  means of `iqa/ssim_tools.c` -> `sycl_ssim_terms.h`, same PR. Guards:
  `test_sycl_float_ssim_exact_contract.py` (device-free, 15 planted
  regressions), `test_sycl_float_ssim_parity` (+ `_large`: `==`; order
  cases on device and through host hook
  `vmaf_sycl_float_ssim_host_means()`; fixture
  `core/test/float_ssim_order_frame.h` is shared byte-identical with
  CUDA and HIP tests: never edit it), `test_sycl_kernel_scratch`. Cost at
  `scale=1`: `T-SYCL-FLOAT-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02`.
  `float_ms_ssim_sycl` takes same terms and adds three sums per scale
  ([ADR-1466](../../../../../docs/adr/1466-sycl-float-ms-ssim-raster-sum.md),
  see [ms-ssim.md](ms-ssim.md)).
- **`float_ssim_sycl` decimation is bit-exact with CPU**
  ([ADR-1370](../../../../../docs/adr/1370-sycl-float-ssim-device-decimation.md)).
  `decimate_sample()` = `iqa_filter_pixel()` at `(x * scale, y * scale)`:
  window rows / columns `r - scale / 2` for `r` in `[0, scale)`,
  `symmetric_index()` = `KBND_SYMMETRIC`, product `sample * tap_weight`
  in fp32 (`tap_weight` = `ssim.c`'s `1.0f / (scale * scale)`, computed on
  host), summed exactly in int64 units of 2^-52, converted once with
  `rounding_mode::rte`. No fp32 accumulation, no FMA path (no adds on
  floats), no reordering matters because sum is integer. Exact only up
  to `SSIM_MAX_EXACT_SCALE` (128); `float_ssim_geometry_supported()` is
  single predicate for `check_context_sycl()` and init, also requiring
  11x11 decimated plane. Plane size = `iqa_decimate_dim()` from
  `iqa/decimate_dim.h` (shared with `iqa/decimate.c`; include-free so this
  C++ TU never parses `convolve.h`). Samples: `picture_copy()`'s layout
  (uint16 at 10 / 12 / 16 bits scaled by exact reciprocal of 4 / 16 /
  256, else uint8). Frame means go through `float_ssim_frame_mean()`,
  which rounds to fp32 like `iqa_ssim()`: removing it breaks `enable_db`
  on near-identical frames. One queue wait per frame, in `collect()`.
  sums those means divide: ADR-1463 bullet above.
  Guards: `test_sycl_float_ssim_parity` (+ `_large`, scales 1-10, 8 / 10 /
  12-bit, odd sizes, `enable_lcs`, gate verdicts),
  `test_gpu_float_ssim_auto_scale_contract`. On rebase: change to
  `iqa_filter_pixel()`, `KBND_SYMMETRIC`, `ssim_low_pass_alloc()` or
  `picture_copy()` scaling changes this kernel in same PR.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_ssim_sycl.cpp` (`float_ssim_sycl`) | `float_ssim.c` + `ssim.c` + `iqa/ssim_tools.c` + `iqa/ssim_accumulate_lane.h` | `test_sycl_float_ssim_parity.c` (+ `_large`), `test_sycl_float_ssim_exact_contract.py` | ADR-1370, ADR-1463 |
