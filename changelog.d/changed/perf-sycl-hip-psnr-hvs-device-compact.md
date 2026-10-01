- **`psnr_hvs_sycl` and `psnr_hvs_hip` compact nonzero terms on the device before readback (ADR-1397).**
  Because $x + 0.0\text{f} == x$ for every float value in the running sum, zero error terms
  contribute nothing to the plane score. Both twins now compute a 64-bit mask of nonzero terms per 8×8 block,
  perform work-group parallel prefix scans, and compact nonzero terms directly on the device into contiguous
  buffers prior to host readback. Readback size drops from ~198.3 MB to ~11.0 MB at 3840×2160 (94.4% reduction),
  from ~49.4 MB to ~1.39 MB at 1920×1080 (97% reduction), and from ~4.35 MB to ~0.21 MB at 576×324 (95% reduction).
  Host summation overhead in `vmaf_psnr_hvs_plane_score_compacted()` shrinks accordingly.
  Throughput at 3840×2160 improves from 39.23 ms to 29.30 ms/frame on Intel Arc A380 (SYCL) and from
  41.18 ms to 35.53 ms/frame on AMD gfx1036 (HIP). Every score on every frame remains bit-identical to
  `--backend cpu` of the same binary at `--precision max`, and the SYCL kernels remain completely scratch-free
  (0 private memory, 0 spills).
