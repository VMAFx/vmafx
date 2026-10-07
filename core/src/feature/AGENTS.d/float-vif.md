---
paths:
  - core/src/feature/float_vif.c
  - core/src/feature/vif_tools.h
invariant: float_vif lint decomposition, run-time Gaussian filter construction, and minimum dimension checks.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Float VIF Lint Decomposition and Run-Time Gaussians

- **Floating-point VIF lint decomposition** (ADR-0141 / ADR-1142):
  `vif.c` keeps ten-plane aligned layout and original convolution,
  decimation, statistic and scale-reduction order. Preserve float intermediate
  values before double score storage. `vif.h` declares all three legacy
  external symbols, including `vifdiff`; do not make them static to satisfy
  per-TU lint. temporal first-frame placeholders and offset/difference/
  previous-frame-copy order are unchanged. Debug dumps write one initialized
  float for each reduced numerator/denominator. See
  [focused investigation](../../../../docs/research/vif-native-lint-2026-09-08.md).
- **`float_vif` Gaussians = run-time `vif_get_filter()`, no table**
  (ADR-0416, #758: upstream on-the-fly filter synced;
  `vif_filter1d_table_s` + `enum vif_kernelscale_enum` gone from
  [`vif_tools.h`](../vif_tools.h)). `float_vif.c::init()` caches
  `vif_get_filter()` output per scale (ADR-0500). Taps = fp64 `exp`,
  fp32 sum, fp32 divide: NOT old table's decimals (26 of 34 taps
  differ, centre tap of scale 0 by 14 ulp). GPU twin must take taps from
  `vif_get_filter()` on host, never literal table: stale table =
  3.8e-5 on Netflix pair (ADR-1412 CUDA fixed, ADR-1422 SYCL fixed, ADR-1444
  HIP fixed; Metal open,
  `T-GPU-FLOAT-VIF-CPU-ARITHMETIC-2026-10-01`). Also load-bearing for
  twins: `VIF_OPT_FAST_LOG2` (`vif_options.h`) makes CPU `log2f`
  polynomial `log2f_approx()`, no libm; `vif_pixel_statistic_s()` keeps
  `vif_sigma_nsq` in `double`; `vif_statistic_s()` sums row by row in
  fp32. Change any of these -> change
  `float_vif_gpu_common.h` (CUDA + HIP twins, ADR-1444; CUDA spelling in
  `cuda/float_vif/float_vif_device.h`) and
  `sycl/sycl_float_vif_math.h` same PR
  (`test_float_vif_device_math`, `test_sycl_float_vif_math` fail until
  they follow).
  [Research-0024](../../../../docs/research/0024-vif-upstream-divergence.md)
  = history of table era.

- **`vif_tools.c` float filters**: `vif_use_avx2_convolution` is only
  place ADR-0504 AVX2-only decision lives (AVX-512 float convolution was
  removed for golden parity — do not re-add it here); `vif_mirror_index` is
  reflect-101 (`-idx` / `2n - idx - 2`) and is shared by all three vertical
  passes and horizontal pass. `vif_pixel_statistic_s` keeps `vif_sigma_nsq`
  as `double` in `log2f` arguments — narrowing it changes promotion.

`float_vif.c`'s guard is derived from `vif_get_min_dim(kernelscale)` —
largest `((filter_width_s / 2) + 1) << s` over four-scale ladder, 16 at
default kernelscale — not from scale-0 filter alone. Do not replace it with
constant.

## Prescaled planes stay inside the int index of `vif_tools.c` (T-PRESCALED-PLANE-INT-INDEX-2026-10-05)

`vif_tools.c` indexes plane as `y * stride + x` in `int`. `init()` refuses
prescaled plane whose rows times stride (in samples) pass INT_MAX, through
`vif_plane_fits_int_index()` in `vif_tools.h`, before it allocates anything
(`init_scaled_plane()` holds scaled-size checks).
`speed.c` and `speed_internal.c` call same helper. Widening index
instead would touch every function of file; plane such prescale
needs is at least 8.6 GB per float buffer. `core/test/test_prescaled_plane_int_index.c`
holds limit (16K accepted at prescale 4, cap refused above 1.414).
