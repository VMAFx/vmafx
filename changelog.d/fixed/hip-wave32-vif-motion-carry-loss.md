- **HIP `vmaf --backend hip` 19-point VIF divergence (AMD RDNA2/RDNA3 wave32).** Two
  stacked bugs in the HIP VIF and motion kernels caused `vmaf --backend hip` to score
  57.335 on the Netflix golden src01 pair while CPU/CUDA/SYCL/Vulkan scored 76.668.

  **Bug 1 (motion):** `motion_score.hip` hardcoded `MS_WARP_SIZE=64` for the warp-stride
  reduction.  AMD gfx1030/gfx1036 (RDNA2/RDNA3) uses wave32 (32 lanes per wavefront);
  the hardcoded stride of 32 in `__shfl_down(x, 32)` reads an out-of-bounds lane,
  producing a zero SAD accumulator.  Fix: replace the compile-time constant with the
  runtime `warpSize` device variable, which returns 32 on wave32 targets.

  **Bug 2 (VIF):** `vif_statistics.hip`'s `wavefront_reduce_i64` split the `int64_t`
  accumulator into two independent 32-bit halves and reduced them separately with
  `__shfl_xor`.  Every carry from the lo-half partial sum (representing `+2^32`) was
  discarded because the hi-half never received it.  For the VIF `x` accumulator
  (per-pixel shift amounts, typically -2 to -16), the carry loss accumulated to
  ~500 billion error across the 576×324 frame, collapsing all four VIF scale scores
  near zero and dragging the SVM output to 57.335.  Fix: reconstruct each neighbour
  lane's `lo`+`hi` words into a full 64-bit value _before_ adding (mirroring the CUDA
  `cuda_helper.cuh::warp_reduce` pattern), so the 64-bit addition's carry propagates
  naturally.

  After fix: VMAF HIP = 76.440 (was 57.335; delta from CPU = 0.228, driven by
  hardware `log2f` precision in the VIF log-coefficient kernel — pre-existing
  limitation, not introduced by this fix).  Motion metrics are bit-exact vs CPU
  (delta = 0.000000).  See [ADR-0688](../../docs/adr/0688-hip-wave32-vif-motion-fix.md)
  and [Research-0688](../../docs/research/0688-hip-raphael-igpu-divergence.md).
