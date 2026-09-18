- **Integer ADM no longer overflows a 32-bit sum on bright 16-bit input.**
  Scale 0's vertical wavelet pass added up four weighted 16-bit samples in a
  signed 32-bit integer, which is undefined behaviour once three neighbouring
  samples reach about 42,000, as in bright HDR or synthetic content. The
  scalar, AVX2 and AVX-512 paths now form that sum in 64 bits. Scores do not
  change: the overflow happened to cancel out on the hardware we test, which
  is also why only a sanitizer build could see it. Upstream Netflix/vmaf has
  the same code.
