---
paths:
  - core/src/feature/sycl/integer_vif_sycl.cpp
  - core/src/feature/sycl/sycl_integer_vif_math.h
  - core/test/test_sycl_vif_parity.c
invariant: integer_vif_sycl.cpp = CPU vif, bit for bit; rd_stride uses ceiling division for odd widths.
---
<!-- markdownlint-disable MD013 MD060 -->
# Integer VIF extractor and kernels

- **`integer_vif_sycl.cpp` rd_stride uses ceiling division for odd widths** (ADR-1034).
  Both `launch_vif_hori_impl` and `launch_vif_fused_impl` (SIMD-16 only, ADR-1830)
  compute downsampled row stride as `(e_w + 1U) / 2U`, not `e_w / 2U`.
  `rd_ref`/`rd_dis` allocation uses `vif_rd_plane_size()` =
  `((w+1U)/2U) * ((h+1U)/2U)` elements. Keep in sync. On rebase: if future PR modifies
  downsampling path, ensure all three sites (two kernel variants + allocation) use
  same ceiling formula. For even widths/heights result identical to
  truncating division.
  **Reader side too** (T-SYCL-VIF-ODD-WIDTH-RD-STRIDE-2026-09-29):
  `enqueue_vif_work_impl` passes scale s > 0 stride `(prev_w + 1U) / 2U`
  it was written with, while `cur_w` stays `prev_w / 2` (CPU floor). Reading
  at `cur_w` skewed scales 1-3 on any odd-width scale (17x17 scale1 0.0962 vs
  CPU 0.0765; 854x480 scale3 9.3e-4 off). Guard: `test_sycl_vif_min_dim`
  (17x17, 853x480 at places=4).
- **`vif_fused=true` alternates two pairs of rd planes
  (T-SYCL-VIF-FUSED-RD-RACE-2026-10-01).** One fused launch reads own scale
  from rd planes AND writes next scale's rd planes; work-groups never wait
  for each other -> read buffer != write buffer, always.
  `vif_rd_output(s, scale)`: scales 0 / 2 write `d_rd_ref` / `d_rd_dis`,
  scale 1 writes `d_rd_ref_alt` / `d_rd_dis_alt` (fused only, sized for
  scale 1's output: `vif_rd_plane_size(w / 2, h / 2)`), scale 3 writes
  nothing; scale s reads `vif_rd_output(s, s - 1)`. Scale 2 overwrites
  pair scale 1 read: safe only because queue in order. Separate
  passes keep one pair (vertical kernel reads, horizontal kernel writes).
  One shared pair again -> scales 1-3 off on every frame from 1920x1080 up
  (BBB 4K 4.9e-4, 200 / 200 frames). Guard: `test_sycl_vif_parity` fused
  cases (fixture, 1920x1080, 1919x1079, 3840x2160 at 8 / 10 bit; `==` CPU
  and separate passes).
- **`integer_vif_sycl.cpp` = CPU `vif`, bit for bit (ADR-1432).** Two parts.
  (1) Host tail = `integer_vif.c` rounding points: `vif_store_residuals()`
  stores each scale's num / den in `float`, `write_scores()` adds
  ROUNDED values for debug sums, emitter divides single precision
  (`.single_precision_ratio = true`). Twin: `vif_scale_sums()` -> two
  `(float)(...)` casts per scale, `vif_score_set()` mirrors
  `write_scores()`. `debug` default = `false` like CPU. (2) Gain terms:
  CPU forms `g = sigma12 / (sigma1_sq + eps)`, `sigma2_sq - g * sigma12`,
  `g * g * sigma1_sq` in fp64, truncates two integers. Kernel calls
  `vmaf_sycl_ivif::gain_terms()` (`sycl_integer_vif_math.h`): ONE integer
  division `sigma12^2 / sigma1_sq` (`divide()`: two fp32 estimates +
  integer fix, no 64-bit divider), `eps` as fp64 sum rounds it
  (`divisor_of()`), zones = fp64 chain's own error (2^-20 for `sv_sq`,
  2^-51 relative for product); undecided sample (1 pixel in 300 000)
  -> `gain_terms_replayed()` = reference's six fp64 ops on `SoftDouble`
  (`sycl_soft_double.h`, shared with `float_vif`). NEVER fp32 gain
  again: it left numerators 1-6 fp32 steps off (3.6e-7). Integer limit:
  `g < L` <=> `sigma12 <= L * sigma1_sq`, product exact; non-integer
  limit at limit -> replay. NO `sycl::mul_hi()` on uint64: wrong
  values in kernel on Arc A380 (icpx 2026.0); `u128_mul()` in 32-bit
  limbs. Scratch (ADR-1395): per-pixel terms = `vif_terms` (seven
  `int32_t`, widened in `dev_reduce_and_accum()`), fused scale-0 kernel
  takes 256-entry register file at SIMD-16 too (`vif_fused_grf_size()`;
  default file spilled 128 B). Arc A380: Netflix 48 / 48, checkerboards
  3 / 3, BBB 4K 200 / 200, 10 / 12 / 16 bit, `debug=true`, limits 1.0 /
  1.2 / 37.5, clip vs itself: identical. Cost 21.46 -> 22.21 ms / 4K
  frame. `integer_vif.c` gain lines change upstream -> change header same
  PR. Guards: `test_sycl_integer_vif_math` (host + device vs fp64
  lines), `test_sycl_vif_parity` (+ `_large`, `_sg32`, `==`),
  `test_sycl_vif_exact_gain_contract.py` (seven planted regressions),
  `test_sycl_vif_float_sums_contract.py`, `test_sycl_kernel_scratch`.
- **`integer_vif_sycl.cpp` minimum frame = 16 px, declared through ADR-1324**
  (T-INTEGER-VIF-TINY-FRAME-GUARD-2026-09-29, maintainer decision: CPU
  fallback). `VIF_MIN_DIM` = max over scales of `(half_width + 1) << s` for
  `vif_fwidth` / `vif_fwidth_rd` (`static_assert` 16): below it consumed tap
  sits more than one reflection outside plane -> device lost.
  `.context_check` returns -ENOTSUP below it, `.context_fallback_name = "vif"`
  -> model dispatch (and CLI twin selection) runs CPU `vif`; direct
  `vif_sycl` fails `init()` with -EINVAL before touching device state. On
  rebase: filter-table change -> update `static_assert`, keep both guards.
- **`integer_vif_sycl.cpp` warning-clean phase boundaries load-bearing.**
  Keep `dev_vert_accumulate`, `dev_hori_item_step`, `vif_init_resources`,
  `vif_configure_device`, `vif_register_graph` as bounded phases. Strict
  C++ profile requires private declarations in anonymous namespaces, while
  HISS-04 applies 60-line limit to each namespace block as well as each
  function; never collapse these blocks or replace with `NOLINT`. Tap loops
  deliberately carry no forced-unroll pragma: oneAPI 2026 emits failed-unroll
  diagnostic for supported target instances and stays free to unroll when
  profitable. Preserve `float` device gain in `VifHoriLaunchParams`,
  arithmetic order inside each phase, cleanup points in three init helpers.
  Base-vs-refactor proof covers default and fused modes on 8-bit and 10-bit
  inputs at zero full-precision delta; rerun both modes after upstream
  conflict.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_vif_sycl.cpp` | `integer_vif.c` | `test_sycl_vif_parity.c` (bit-exact, every output, 8 / 10 bit), `test_sycl_integer_vif_math.c` | ADR-0868 (round 1), ADR-1432 |

| Kernel TU | Parity test | ADR |
|---|---|---|
| `integer_vif_sycl.cpp` | `test_sycl_vif_parity.c` | ADR-0868 |
