- Fail the frame when any of the twelve non-finite score paths found in the
  metric-engine sweep would otherwise publish a plausible value. VIF and ADM
  no longer turn NaN into their configured minimum, SSIM and MS-SSIM no longer
  turn it into `max_db`, SSIMULACRA2 no longer turns it into the perfect
  `100.0`, TransNet no longer reports “not a boundary”, and the aggregate VMAF
  piecewise mapping no longer reports `0.0`. SSIMULACRA2's scalar, SIMD, CUDA,
  HIP, SYCL and Metal hosts share the same failure semantics. See ADR-1302.
- The Netflix golden gate is unchanged at `271 passed, 12 skipped`, and the
  clang-tidy ratchet is unmoved, so no pinned score and no debt count moves.
