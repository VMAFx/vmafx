- **Restore exact high-bit-depth `float_moment` reduction on Metal.** PR #1067
  silently restored the old float32 workgroup partials after PR #1029 had
  removed them, so 10/12/16-bit second moments could drift from the CPU before
  host accumulation. Metal now reduces raw moments exactly in uint64
  threadgroup scratch, exports carry-safe uint32 lo/hi pairs, and applies the
  CPU's bit-depth scaler on the host. A 10-bit CPU-vs-Metal gate and a
  platform-independent source/numerical contract prevent another silent
  revert.
