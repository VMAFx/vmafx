- **perf(cuda/motion)**: multi-resolution ncu profile of `calculate_motion_score_kernel_8bpc`
  at 576p, 1080p, and 4K (Research-0760). Confirmed CUDA motion is dispatch-bottlenecked
  at all resolutions below 4K (GPU busy <1% of wall time). Kernel-only compute at 4K
  achieves 48% SM throughput with 42 waves and 82% occupancy. Top-3 optimizations
  identified: (1) multi-frame SAD batching (+2× at 576p, +4× expected), (2) separable
  2-pass filter (−40% kernel duration at 4K), (3) `cp.async` tile prefetch
  (−10–15% kernel duration). No code changes in this commit — research digest only.
