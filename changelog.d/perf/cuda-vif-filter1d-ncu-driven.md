## perf(cuda): VIF filter1d horizontal kernel register pressure and cache routing

`filter1d_8_horizontal_kernel_2_17_9` (scale-0, 8-bit, 17-tap horizontal VIF pass):

- Added `__launch_bounds__(128, 10)` to the kernel instantiation macro, reducing
  register count from 56 to 48 per thread on sm_89 (RTX 4090). Theoretical
  occupancy improves from 75% to 83.3%.

- Added `__ldg()` on the 7 read-only tmp-channel global loads in the smem-fill
  phase, routing them through the read-only L1 cache. Beneficial at production
  resolutions (≥ 1080p) where the tmp channels exceed L2 capacity per frame.

Both changes are semantically neutral: same integer arithmetic, scores match CPU
reference within ADR-0214 places=4 tolerance. See ADR-0743.
