---
paths:
  - core/src/feature/hip/ciede_hip.c
  - core/src/feature/hip/ciede_hip.h
  - core/src/feature/hip/integer_ciede/ciede_score.hip
invariant: ciede_hip performs CPU arithmetic in fp32 pairs with libm residual handling.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# ciede_hip = CPU arithmetic in fp32 pairs, libm residual (ADR-1448)

- NOT exact twin. `LIBM_TWINS` `ciede`: `hip` = 1e-9
  (`scripts/ci/cross_backend_calibration.py`). gfx1036: 115 of 178 frames
  identical, rest <= 1.4e-11.
- Arithmetic = `../ciede_ff_math.h` (`pixel()`) + pair functions
  `../ff_math.h`, shared with `ciede_sycl` (ADR-1436): `ciede.c` statement
  for statement, fp64 value = fp32 pair, fp64 libm call = pair function.
  Change there = change both twins; re-measure A380 + gfx1036 same PR.
- HIP primitives = `integer_ciede/ciede_hip_math.h` + `../ff_pair.h`: plain
  fp32 `+ - * /` (strict FP list), `fmaf()` for `two_prod`, root estimates
  `sqrtf` / `cbrtf` / `expf(0.2f * logf(x))`. NOT `powf(x, 0.2f)`: same
  values, 7 of 50 ms per 1080p frame. NO fp64 math on device: CUDA
  fp64 statements = 318 ms per 1080p frame (17x), never merged.
- Kernel `integer_ciede/ciede_score.hip` (built `-std=c++20`,
  `hip_kernel_extra_args`): one thread per pixel, float stored at raster
  position, no reduction. Constants = `constexpr kCiedeConstants[4]` (8, 10,
  12, 16 bit), host passes index `ciede_hip_depth_index()`; tables =
  module constants.
- Host: readback one float per pixel, `ciede_frame_sum()`
  (`../ciede_frame_sum.h`, shared with CUDA + SYCL hosts) raster order into
  one double, `45. - 20. * log10(de00_sum / (w * h))`.
- Residual measured per pixel (437 M pixels): 2 206 one float step off =
  glibc `powf` not correctly rounded; 8 off by 1-9 steps = pair (48 bits) vs
  fp64 (53) deciding intermediate float. Another device or libm:
  re-measure before trusting 1e-9.
- Cost 2.7x: 1080p 18.6 -> 49.6 ms, 4K 75.6 -> 210.1 ms (CPU 16 threads
  136). 1080p split: Lab conversions 21.9, colour difference 25.5, upload +
  readback + host sum 2.2. `T-HIP-CIEDE-EXACT-THROUGHPUT-2026-10-02`.
- Guards: `test_hip_ciede_parity` (+ `_large`, 1e-8), `test_hip_ciede_math`
  (host: pair functions vs extended-precision libm, pixel vs fp64
  statements; `test_sycl_ciede_math.c` + `test_hip_ciede_math_probe.cpp`),
  `test_hip_ciede_exact_contract.py`.
