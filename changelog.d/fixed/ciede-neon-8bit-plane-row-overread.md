- `ciede_preprocess_8_neon` read past the end of every plane row on
  aarch64. The vector loop stepped four pixels per iteration but issued
  an eight-byte `vld1_u8`, so its final iteration touched
  `4 - (w % 4)` bytes beyond `buf + w` — four whole bytes on every
  width that is a multiple of four. `ciede.c` passes these kernels a
  bare row pointer into a `VmafPicture` whose stride is only
  `ceil(w / 64) * 64`, so at the common multiple-of-64 widths (576,
  1280, 1920, 3840) the row has no padding at all and the last row of
  the V plane sits at the end of the allocation: a four-byte heap
  over-read on every frame. The loop now steps eight pixels and
  consumes all eight bytes it loads, which also doubles the vector
  throughput and brings the kernel in line with the AVX2 counterpart.
  Scores are unaffected — the over-read bytes landed in lanes 4-7 and
  were discarded — so this is a memory-safety fix, not a numeric one;
  a full 48-frame `ciede` run of the 576x324 Netflix pair under
  qemu-aarch64 is byte-identical before and after.
  New `core/test/test_ciede_neon.c` closes the coverage gap that let
  this ship: `test_ciede_simd_parity` was gated to x86, so neither NEON
  kernel had a test on any architecture. It asserts bit-exact float
  parity against the `ciede.c` scalar fallback and, with a `PROT_NONE`
  guard page, that neither kernel reads past `buf + w`.
  `ciede_preprocess_16_neon` was already correct on both counts.
