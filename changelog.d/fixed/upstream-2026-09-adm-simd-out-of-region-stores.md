- **Integer ADM SIMD kernels no longer store outside their output bands**
  (ports of upstream Netflix/vmaf `03b5562c5` and `ea012e387`).
  `adm_decouple_avx2` computed its 8-wide tail bound from column 0 although its
  loop starts at the left border. Its last store therefore ran up to seven
  samples past the decouple region, and for band widths 32 and 40 past the end
  of the row. No consumer reads those samples, so x86 scores are unchanged.
  `adm_dwt2_8_neon` had no horizontal tail and stored one sample past every
  band row. On the last row that sample overwrote the first element of the
  next band in the shared ADM buffer with a value computed from over-read
  scratch. On AArch64 this changed `integer_adm_scale0` for frame widths 24,
  32 and 40 at heights below 50, differently from run to run. Both loops now
  stop at a bound that keeps every store inside the band, and a scalar tail
  handles the remainder. Every NEON-dispatched width is bit-exact with the
  scalar reference, and scores for larger frames, including every Netflix
  golden fixture, are unchanged.
