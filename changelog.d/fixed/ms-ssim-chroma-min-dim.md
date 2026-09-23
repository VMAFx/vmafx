- **`float_ms_ssim=enable_chroma=true` no longer dies mid-run on small 4:2:0
  input.** The pyramid minimum was checked against luma only, so a 4:2:0 input
  between 176x176 and 352x352 passed init and then failed inside upstream
  `ms_ssim.c`, which prints `error: scale below 1x1!` to stdout and returns 1 —
  surfacing as a bare "problem with feature extractor" and no output file at all.
  This fired on the repository's own 576x324 Netflix fixture, whose chroma is
  288x162. Both the CPU and SYCL twins now check every scored plane and refuse at
  init with the chroma size they measured and the luma resolution that would work.
