- **test**: add `test_motion_neon` — bit-exact parity coverage for
  `x_convolution_16_neon` (`core/src/feature/arm64/motion_neon.c`), the one
  kernel that file exports and the last motion kernel with no unit test on any
  architecture. The NEON output is compared byte-for-byte against a scalar
  transcription of the upstream `integer_motion.c::x_convolution_16`, the same
  reference `test_motion_avx512_parity` holds the AVX-512 twin to, across 21
  widths (covering every `(width - 5) % 8` residue, so the 8-wide vector body,
  its scalar tail, and the degenerate `right_edge <= left_edge` widths are all
  exercised), 5 heights, and 4 input patterns including full-range `0xFFFF`.
  Strides differ from the width and from each other, and the destination
  carries a sentinel border that catches out-of-bounds writes. Gated on
  `ARCH_AARCH64`, registered in suites `fast` + `simd`. The kernel passed
  unmodified — no production code changed.
