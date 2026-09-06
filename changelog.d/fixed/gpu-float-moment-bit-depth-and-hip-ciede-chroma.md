- **`float_moment` on CUDA, SYCL and HIP was 4x/16x/256x too large above 8 bpc.**
  The CPU reference runs `picture_copy()` first, which divides every
  high-bit-depth sample by 4 (10 bpc), 16 (12 bpc) or 256 (16 bpc); the three
  GPU twins accumulated the raw codeword and only divided by the pixel count.
  On the Netflix 576x324 pair at 10 bpc, CUDA reported `float_moment_ref1st`
  247.715 against the CPU's 61.929 and `ref2nd` 78969.8 against 4935.6. No
  parity fixture in the tree was above 8 bits, so nothing could see it. Fixed
  by dividing the exact device sums by the scaler (and scaler squared for the
  second moment) on the host — bit-identical to the CPU at 10 and 12 bpc — and
  by making the parity fixtures bit-depth generic with 10-bit variants on all
  three backends. HIP, which had no `float_moment` gate at all, gets one.

- **`ciede_hip` read past its chroma staging on odd luma dimensions.** It sized
  the staging with `w >> 1` where `picture.c` allocates chroma as
  `(w + 1) >> 1`; at 577 wide the last chroma column was never uploaded and
  the kernel read the first sample of the next row for it. Fixed by using the
  same ceil formula, with an odd-size (577x325) parity variant added.
