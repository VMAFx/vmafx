- **`float_ssim` runs on the HIP device at 1080p and 4K (ADR-1405).**
  `float_ssim_hip` implemented scale 1 only, so pictures with a short side of
  384 px or more were computed on the CPU, with the warning
  `float_ssim_hip cannot run 3840x2160 8-bit pictures with these options`.
  The twin now decimates on the device at the automatic scale and every
  explicit one, with the CPU's reduced planes bit for bit, and falls back only
  when the decimated plane is smaller than the 11x11 SSIM window. On a gfx1036
  `float_ssim` is within 1.8e-6 of the CPU at 3840x2160 and 4.3e-6 at
  1920x1080; a 4K frame takes 5.5 ms against 11.9 ms for the CPU extractor on
  16 threads and 19.7 ms for the previous fallback, a 1080p frame 2.0 ms
  against 2.7 ms and 6.6 ms. Scale-1 scores are unchanged.
