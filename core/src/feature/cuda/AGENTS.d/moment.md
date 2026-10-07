---
paths:
  - core/src/feature/cuda/integer_moment_cuda.c
  - core/src/feature/cuda/integer_moment_cuda.h
  - core/src/feature/cuda/integer_moment/moment_score.cu
invariant: float_moment_cuda reproduces CPU float_moment bit for bit, past 2^53 units too.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# `float_moment_cuda` = CPU `float_moment`, bit for bit (ADR-1453, ADR-1497)

- CPU: `moment.c` squares each sample in `float`, adds floats into ONE
  double per output. Kernel (`integer_moment/moment_score.cu`): four
  `uint64` sums; second sums add `sample_square<T>()`: `uint16_t` ->
  `moment_float_square()` = one `__fmul_rn()` product of raw sample, as
  integer below 2^32 (= CPU term in units of 1 / scaler^2; = integer square
  up to 12 bit, rounded to 24 bits at 16); `uint8_t` -> integer square (same
  number). Host: CPU's two divisions (`moment_cuda_scaler()`).
- NEVER `r * r` in integers for 16bpc samples (1.0e-4 off at 16 bit), never
  fp64 square, never plain `*` (intrinsic = contract).
- Exact sum = CPU's while <= 2^53 units (every frame <= 2^21 pixels, every
  8/10/12-bit frame). Past it CPU rounds per add: on frame that can get
  there (`vmaf_moment_sum_may_round()`) `moment_cuda_dispatch_sum()` launches
  four kernels of `feature/float_moment_sum_gpu.h` after frame kernel
  (ADR-1497; page `../../AGENTS.d/float-moment-sum.md`); they write CPU's
  sums into accumulators 2 and 3. Row buffers owned here
  (`vmaf_cuda_buffer_free_owned`). NEVER skip them for 16-bit frames above
  2^21 pixels.
- 16-bit Netflix fixture = 8-bit shifted left, shows nothing: use full-range
  content.
- Guards: `test_cuda_float_moment_parity` (+ `_large`; cases in
  `core/test/float_moment_twin_parity.h`, shared with SYCL and HIP tests; `==`
  past 2^53 and at 2^53 boundary), `test_cuda_float_moment_exact_contract.py`,
  `test_float_moment_sum_contract.py`.
