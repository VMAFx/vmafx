---
paths:
  - core/src/feature/hip/float_moment_hip.c
  - core/src/feature/hip/float_moment_hip.h
  - core/src/feature/hip/float_moment/moment_score.hip
invariant: float_moment_hip matches CPU bits on every frame, past 2^53 units too.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# float_moment_hip = CPU bits (ADR-1447, ADR-1497)

- Exact twin `float_moment` (`scripts/ci/exact_twins.d/float_moment.hip`);
  gfx1036: 250 of 250 frames, four outputs each.
- CPU second moment: square in `float` (`pic_ * pic_`), added in `double`.
  16 bpc: float square = integer square rounded to 24 bits. Kernel adds
  `moment_float_square()` (one fp32 product -> integer < 2^32). Never
  `r * r` in integers at 16 bpc, never fp64 square.
- 8 bpc kernel: integer square = float square (16 bits), unchanged.
- Sum = exact uint64 in units of 1 / scaler^2. Host: `(double)sum /
  scaler^2 / pixels`, CPU's two divisions, this order.
- CPU sum exact <= 2^53 units: every frame <= 2^21 pixels, every 8 / 10 / 12
  bit frame. Past it (16 bit, moment * pixels >= 2^37) CPU rounds per add:
  on frame that can get there (`vmaf_moment_sum_may_round()`)
  `moment_hip_launch_sum()` runs four kernels of
  `feature/float_moment_sum_gpu.h` (compiled into `moment_score.hip`) after
  frame kernel, on shared planes; they write CPU's sums into
  accumulators 2 and 3 (ADR-1497). Row buffers: raw `hipMalloc`, freed with
  module. Cost on gfx1036: +7.2 ms per 16-bit 4K frame past 2^53,
  `T-GPU-FLOAT-MOMENT-EXACT-SUM-COST-2026-10-03`.
- Metal twin still adds integer squares:
  `T-GPU-FLOAT-MOMENT-16BIT-SQUARES-2026-10-02`.
- Guards: `test_hip_float_moment_parity` (+ `_large`; cases of
  `core/test/float_moment_twin_parity.h`, `==` past 2^53),
  `test_hip_float_moment_exact_contract.py`, `test_float_moment_sum_contract.py`.
