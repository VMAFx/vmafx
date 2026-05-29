## Performance

- `adm_decouple_avx512`: prefetch `adm_div_lookup` LUT entries 2 iterations ahead
  into L2 before each `vpgatherdd` cluster. The 256 KB LUT exceeds L1 capacity and
  was causing frequent L2/L3 misses that accounted for 66.5 % of the function's cycles
  (2.31 % of total VMAF wall time). Measured improvement on BBB 1080p: −5.8 % total
  wall time (8-run mean 9603 ms → 9049 ms). Bit-exact with scalar reference. (ADR-0502)
