- Fixed out-of-bounds read (UB / ASan SEGV) in the 5-tap separable Gaussian
  motion filter when input frames are smaller than 3×3 pixels. The reflect-101
  mirror formula `height - (i_tap - height + 2)` requires `height ≥ 3`; for
  1×1 or 2×N inputs it produced a negative index. All three CPU motion
  extractors (`motion`, `motion_v2`, `float_motion`) and all GPU backend
  equivalents (CUDA, SYCL, Vulkan, HIP) now reject frames below 3×3 at
  `init()` with `-EINVAL` and a human-readable diagnostic message instead of
  silently reading garbage memory. Regression test `test_motion_min_dim`
  (13 cases) added. Surfaced by the round-7 stability agent (Research-0094).
