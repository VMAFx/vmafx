---
paths:
  - core/src/feature/float_psnr_rows.h
invariant: float_psnr twins add each row's exact sum into double in CPU's row order.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_psnr's sum of rows (ADR-1499)

- `float_psnr_rows.h::vmaf_float_psnr_row_noise()`: CUDA, SYCL and HIP
  hosts' only form of `float_psnr.c::extract()`'s `noise_` (before `/ (w * h)`),
  in units of 1 / scaler^2: each row's segment sums added in `uint64`,
  rows added into ONE double, row 0 first.
- Why it is CPU's bits on every input: row's terms are integers below
  2^32, row has at most 2^15, so every partial sum inside row is exact
  in double in any order (every `noise_line()` variant); only adds of
  rows round (past 2^53 units), and helper makes same adds of
  same values in same order; power-of-two scale changes nothing.
- kernels' blocks / work-groups must each lie in ONE row (256 x 1).
  NEVER add segments of different rows before double add, never round
  frame total once, never reorder rows.
- change to `float_psnr.c`'s row loop or `noise_line()` (e.g. float
  accumulator) changes this header, `test_float_psnr_rows` and three
  twins in same PR.
