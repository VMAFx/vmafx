- **AVX-512 `integer_adm_scale0` now matches the scalar path on frames 17 to
  32 pixels wide.** At those widths one of scale 0's right shifts is by zero
  bits, and the AVX2 and AVX-512 paths computed its rounding term by
  converting infinity to an integer, which is undefined behaviour. The
  AVX-512 build turned it into `0xFFFFFFFF` and scored scale 0 up to 0.01
  away from scalar; the AVX2 build happened to produce the correct 0. Both now
  use the scalar path's guarded helper, so the scalar, AVX2, AVX-512 and NEON
  paths give identical scores at every such width. Scalar and AVX2 scores do
  not change, and neither does any frame wider than 32 pixels.
