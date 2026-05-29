- **perf(cuda)**: `ms_ssim_decimate` shared-memory tiling — 81 global/L2 reads per
  output pixel converted to L1 hits via a cooperative `(2·BLOCK_X+8) × (2·BLOCK_Y+8)`
  float tile per CTA (3936 B smem). `mirror_idx` modulo boundary is now applied only
  during the tile load phase, not in the hot 9×9 convolution loop. Estimated DRAM
  throughput reduction: 30–40 % at 1080p+. (ADR-0744 Opt A)
- **perf(cuda)**: `adm_cm_line_kernel_8` register reduction via
  `__launch_bounds__(128, 8)` — hints ptxas to target ≤64 regs/thread (matching the
  fused scale 1–3 kernel), raising theoretical occupancy from 33 % to ~67 % on Ampere.
  (ADR-0744 Opt B)
