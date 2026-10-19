---
paths:
  - core/src/feature/sycl/integer_ms_ssim_sycl.cpp
  - core/test/test_sycl_ms_ssim_parity.c
invariant: integer_ms_ssim_sycl.cpp = CPU arithmetic, type for type; honours enable_chroma, enable_lcs, enable_db.
---
<!-- markdownlint-disable MD013 MD060 -->
# Multi-Scale SSIM extractor and kernels

- **`integer_ms_ssim_sycl.cpp` honours `enable_chroma` option parity**
  (ADR-0526, ADR-0583). `enable_chroma`
  option (default `false`) clamps `n_planes` to 1 in `init_fex_sycl` when
  set to `false`, to 3 otherwise (except YUV400P which always forces 1).
  Chroma geometry uses picture allocator's ceil subsampling, so
  176x176 chroma minimum maps to exact 351x351 4:2:0 luma minimum;
  init error suggestion uses that exact inverse. Submit, computation and
  publication iterate every active plane, so `enable_chroma=true` dispatches
  Y, Cb and Cr today. On rebase: keep default, YUV400P clamp and
  three-plane dispatch aligned with CPU and Metal MS-SSIM extractors.
- **`integer_ms_ssim_sycl.cpp` honours `enable_lcs`, `enable_db`,
  `clip_db` GPU option parity** (ADR-0243, ADR-1078). When
  `enable_lcs=true`, emits 15 extra metrics
  (`float_ms_ssim_{l,c,s}_scale{0..4}`). When `enable_db=true`,
  returns `-10*log10(1 - ms_ssim)` instead of raw linear score;
  `clip_db=true` derives geometry-dependent `max_db` ceiling from frame
  dimensions and bit depth, then caps dB-domain output at that ceiling
  (ADR-1221). It never clamps linear score to `[0, 1]`.
  All three options default to `false` — output at default settings
  numerically identical to pre-ADR-1078 binary. Metric ordering
  and `places=4` cross-backend contract = part of public API
  surface. See
  [../../AGENTS.md §"MS-SSIM `enable_lcs` GPU contract"](../../../AGENTS.md).
- **`integer_ssim_sycl.cpp` and `integer_ms_ssim_sycl.cpp` are
  self-contained submit/collect** — do **not** register with
  `vmaf_sycl_graph_register`. `integer_ms_ssim_sycl.cpp` needs float
  [0, 255] intermediates from `picture_copy()`; `float_ssim_sycl` uploads
  its own raw luma and does that scaling on device (ADR-1370).
  `ciede_sycl` TU follows same pattern. **On rebase**: do not
  "consolidate" these into graph register — precision posture
  load-bearing. Reading shared frame from `float_ssim_sycl` is
  separate decision (it would also serve `vmaf_read_pictures_sycl()`).
- **`picture_copy()` channel parameter** — `integer_ms_ssim_sycl.cpp`
  passes `channel=0` per d3647c73 prerequisite port
  (`integer_ssim_sycl.cpp` no longer calls `picture_copy()`, ADR-1370). See
  [../../AGENTS.md §"`picture_copy()` carries `channel`
  parameter"](../../../AGENTS.md).
- **`integer_ms_ssim_sycl.cpp` waits once per frame (ADR-1363).** Each
  (plane, scale) owns span `partial_offset[plane][scale]` of
  `d_partials` / `h_partials` (`[l x groups][c x groups][s x groups]`, int64
  since ADR-1414); `submit()` enqueues pyramid and every scale's
  `enqueue_scale_lcs` plus one copy of whole buffer, and `collect()`
  waits once and sums each span (`sum_scale_lcs`). **On rebase**: do not
  share one partials buffer across scales again (reuse is what forced
  per-scale wait); horizontal workspace may be shared because
  queue is in order.
- **`integer_ms_ssim_sycl.cpp` = CPU arithmetic, type for type (ADR-1414).**
  Four things, each one regression if undone:
  (1) `decimate_pixel()`: every tap `sycl::fma(sample, tap, acc)`, rows
  first, then nine row sums = `ms_ssim_decimate.c` (`vmaf_fmaf_exact`).
  Plain `acc += sample * tap` is NOT contracted in this TU (ADR-1367) ->
  1e-6 off. (2) Window sums: `add_horizontal_tap` / `add_vertical_tap` /
  `round_moments` from `sycl_ssim_terms.h` = `iqa_convolve()`'s fp32
  products summed in fp64, as fp32 pairs, one rounding per pass. (3)
  `ssim_float_parts()`: fp32 variances + clamp, fp32 denominators, s =
  `div_rn` fp32 quotient, C3 = C2 / 2.0f; `ssim_double_terms()`: l and c =
  CPU's fp64 quotients in 64-bit integers (`sycl_soft_signed.h`).
  (4) Sums ([ADR-1466](../../../../../docs/adr/1466-sycl-float-ms-ssim-raster-sum.md),
  method of ADR-1463): `MsSsimLcsKernel` = one work-item per window,
  stores lv bits at `d_terms[offset + i]`, cv bits at
  `d_terms[window_count + offset + i]`, fp32 sv at `d_structure[offset + i]`
  (`window_offset[plane][scale]`, raster order, NO reduction; 20 B / window:
  219 MB at 4K); `submit()` enqueues both copies, `collect()` waits once;
  host `ssim_lcs_sums()` = three doubles in index order (no product: ms_ssim.c
  does not use it), mean `(double)(float)(sum / pixels)` (= `iqa_ssim()`
  float return), `combine_ms_ssim()` with `fabs()` on l, c, s. Kernel shape
  `VmafSyclKernelShape<16, 256>`, window function flattened (ADR-1395).
  `sycl_ssim_terms.h` is shared with `float_ssim_sycl`
  (`integer_ssim_sycl.cpp`): ONE copy of arithmetic, no private
  `ssim_double_terms` / `ssim_float_parts` / `add_*_tap` / `ssim_lcs_sums`
  in either TU (contract test). Host-side functions of header use
  `double`; never call them from kernel (ADR-0220). Exact by construction
  since ADR-1466. Before (ADR-1414): pair terms -> `term_fixed()` int64 ->
  `reduce_over_group` -> host `FixedSum` = exact sum, NOT CPU's
  running double; `float_ms_ssim_l_scale0` one float step off on noise
  pair seed 2437157 (176x176). Never bring reduction, pair terms for
  stored term or another host order back. Score vs GCC build: host
  `pow()` of icx build (libimf) can differ by 1.1e-16, not device.
  Same-binary CPU on AVX-512 host needs #1706. Scratch-free (ADR-1395).
  `EXACT_TWINS`: `float_ms_ssim`, `float_ms_ssim_lcs`: `sycl`. Guards:
  `test_sycl_ms_ssim_parity` (+ `_large`; `==` on 18 outputs x 3 frames,
  runs FIRST in binary so its scalar-CPU cpumask precedes
  process-wide SSIM dispatch install; order cases: noise pair, which
  fails on old twin, and `core/test/float_ms_ssim_order_frame.h`, shared
  byte-identical with CUDA and HIP tests, never edit it; old SYCL
  twin passed that one), `test_sycl_kernel_source_contract.py`. Cost:
  `T-SYCL-FLOAT-MS-SSIM-RASTER-SUM-THROUGHPUT-2026-10-02`.

- [ADR-0243](../../../../../docs/adr/0243-enable-lcs-gpu.md) — MS-SSIM
  `enable_lcs` GPU contract.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_ms_ssim_sycl.cpp` | `ms_ssim.c` | `test_sycl_ms_ssim_parity.c` (+ `_large`; bit-exact, 18 outputs x 3 frames) | ADR-0884 (round 2), ADR-1414, ADR-1466 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_ms_ssim_sycl.cpp` | `test_sycl_ms_ssim_parity.c` | ADR-0884 |
