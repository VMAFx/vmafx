- **`psnr_hvs` on aarch64 returns the scores of the scalar path and of an
  x86-64 build.** `calc_psnrhvs_neon()` multiplied the two `float` factors of
  the masking threshold as `float`; the scalar reference and the AVX2 function
  multiply them as `double` (`sqrt((double)mask * variance_ratio) / 32`). The
  rounded product put the threshold one `float` step off on about one 8x8
  block in twenty of real content (181 of 3772 luma blocks of frame 18 of the
  Netflix pair). Most differences vanish in the running sum, so NEON differed
  from scalar on 29 of 708 scores, on 14 of 177 frames of nine fixtures, by at
  most 5.7e-7 dB; it now differs on none, under GCC and under clang, and the
  aarch64 scores equal the x86-64 ones. x86-64 scores and the scalar path are
  unchanged. The new `test_psnr_hvs_dispatch_invariance` scores 165 picture
  pairs through the public API with the host's instruction set and with every
  flag masked and compares the four outputs bit for bit, on x86-64 and aarch64;
  with the old kernel it reports 50 differing scores. See
  [`psnr_hvs`](docs/metrics/psnr-hvs.md#cpu-instruction-sets).
