---
paths:
  - core/src/feature/sycl/integer_moment_sycl.cpp
  - core/test/test_sycl_float_moment_parity.c
invariant: float_moment_sycl = CPU float_moment, bit for bit past 2^53 units too; integers only; no scratch.
---
<!-- markdownlint-disable MD013 MD060 -->
# Float moment extractor and kernels

- **`float_moment_sycl` = CPU `float_moment`, bit for bit
  ([ADR-1449](../../../../../docs/adr/1449-sycl-float-moment-cpu-float-squares.md)).**
  CPU: `moment.c` squares each sample in `float`, adds floats into ONE
  double per output. Kernel (`integer_moment_sycl.cpp`): four `int64`
  sums; second sums add `moment_float_square()` = one fp32 product of
  raw sample, as integer below 2^32 (= CPU's term in units of 1 / scaler^2;
  = integer square up to 12 bit, rounded to 24 bits at 16). Host: CPU's two
  divisions. NEVER `r * r` in integers (1.0e-4 off at 16 bit), never
  fp64 square. Exact sum = CPU's while <= 2^53 units (every frame <= 2^21
  pixels, every 8/10/12-bit frame). Past it CPU rounds per add: on frame
  that can get there `launch_moment_sum()` runs four kernels with lane
  steps of `feature/float_moment_sum.h` (row totals, plans, row units, walk;
  ADR-1497) and writes CPU's sums into accumulators 2 and 3. Planes picked
  by value (`moment_sum_row()`), never `a.luma[plane]` (private array =
  scratch). Header included with `VMAF_ORDSUM_FUNC static
  VMAF_SYCL_ALWAYS_INLINE` (call in kernel = scratch frame, ADR-1395).
  16-bit Netflix fixture = 8-bit shifted left, shows nothing: use full-range
  content. Guards: `test_sycl_float_moment_parity` (+ `_large`),
  `test_sycl_float_moment_exact_contract.py`. Reduction = four atomics per
  pixel, 28.7 ms per 4K frame on A380:
  `T-SYCL-FLOAT-MOMENT-PER-PIXEL-ATOMICS-2026-10-02`.

| SYCL TU | CPU TU | Parity test | ADR |
|---|---|---|---|
| `integer_moment_sycl.cpp` (`float_moment_sycl`) | `float_moment.c` | `test_sycl_float_moment_parity.c` (+ `_large`; bit-exact, 8 to 16 bit, past 2^53 too) | ADR-0957 (round 4), ADR-1449, ADR-1497 |
