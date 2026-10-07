---
paths:
  - core/src/feature/sycl/float_vif_sycl.cpp
  - core/src/feature/sycl/sycl_float_vif_math.h
  - core/test/test_sycl_float_vif_parity.c
invariant: float_vif_sycl.cpp = CPU arithmetic, bit for bit; options must be captured, not hardcoded.
---
<!-- markdownlint-disable MD013 MD060 -->
# Float VIF extractor and kernels

- **`float_vif_sycl.cpp` = CPU arithmetic, bit for bit (ADR-1412,
  ADR-1422).** Four CPU properties copied: (1) taps =
  `vif_get_filter()` on host (`init_vif_taps()`), passed BY VALUE
  (`VifTaps`), indexed only by unrolled-loop constants (run-time index ->
  private memory); no tap literal in TU. (2) `log2` =
  `log2_approx()` (`sycl_float_vif_math.h`), CPU `log2f_approx()` op for
  op; never `sycl::log2`. (3) CPU's two fp64 expressions
  (`1 + g*g*s1 / (sv + nsq)`, `1 + s1 / nsq`; `vif_sigma_nsq` is
  `double`) = `one_plus_ratio()`: exact fp32 pair (`ff_div`, `ff_add`),
  and within 2^-12 step of rounding boundary (1 sample in 1650)
  fp64 ops replayed in int64 (`SoftDouble`). Pair alone wrong on 85 of
  8.4e9 quotients (ties: reference rounds twice): do NOT drop replay.
  Struct selects (`cond ? a : b` on `SoftDouble`) = 512 B private memory:
  select fields. (4) sums: `vif_row_sums()` ONE work-item per row
  (`sycl::range<1>(height)`, sub-group 16), plain left-to-right loop;
  host `sum_vif_rows()` adds rows in fp32. No group / sub-group / atomic
  reduction in TU. Per scale: filter kernel (stores `sigma1_sq`,
  `sigma2_sq`, `sigma12`) -> `FloatVifStatisticKernel` (one work-item per
  pixel, sub-group 16, default register file; replaces first two planes
  by num / den terms) -> row sums; one readback per frame. Statistic
  inside filter kernel spills (1440-1600 B) at default register file;
  with large register file scratch-free but 29.4 ms / 4K frame vs 24.1:
  do not merge back. Statistic at sub-group 32 default register file
  spills (480-640 B). Arc A380: Netflix pair 48 / 48, checkerboards 3 / 3,
  BBB 4K 200 / 200, 10 / 12 / 16 bit, `debug=true`, non-default options:
  identical. Cost: 20.54 -> 23.95 ms / 4K frame, +100 MB device memory
  (`T-SYCL-FLOAT-VIF-EXACT-THROUGHPUT-2026-10-01`). `vif_get_filter()`,
  `VIF_OPT_FAST_LOG2` / `log2f_approx()`, `vif_pixel_statistic_s()`,
  `vif_statistic_s()` change upstream -> change header same PR.
  `EXACT_TWINS` lists `float_vif`: `sycl`. Guards:
  `test_sycl_float_vif_math` (host + device vs `vif_statistic_s()`, fp64
  expressions, twelve witnesses), `test_sycl_float_vif_parity` (+
  `_large`, `==`, cases in `float_vif_twin_parity.h`),
  `test_sycl_float_vif_exact_contract.py` (eleven planted regressions),
  `test_sycl_kernel_scratch`.
- **`float_vif_sycl.cpp` options must be captured, not hardcoded**
  (ADR-1217) — see canonical note in
  [`../cuda/AGENTS.md`](../../cuda/AGENTS.md). Compute kernel captures
  `vif_sigma_nsq`, `vif_enhn_gain_limit` and host-derived
  `sigma_max_inv` from `launch_compute`'s parameters; must not
  re-declare them as kernel-local constants.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `float_vif_sycl.cpp` | `float_vif.c` | `test_sycl_float_vif_parity.c` (bit-exact, every output, 8 / 10 bit), `test_sycl_float_vif_math.c` | ADR-0946 (round 3), ADR-1422 |
