- **The CUDA, HIP and Metal integer ADM twins no longer overflow a 32-bit sum
  on bright 16-bit input.** Their scale-0 vertical wavelet pass added up four
  weighted 16-bit samples in a signed 32-bit integer, as the CPU path did
  until it was fixed. That sum is now formed in 64 bits on every twin; the
  SYCL twin already did. Scores do not change, and neither does throughput.
