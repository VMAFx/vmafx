- **`psnr_cuda`, `float_moment_cuda` and the CUDA motion SAD kernel spend
  far less GPU time per frame (ADR-1392).** The kernels added one atomic per
  warp to their 64-bit accumulators, which serialised them in the L2; they
  now add one per block and accumulator. PSNR and moment threads sum eight
  coalesced pixels each, PSNR selects its plane without copying both
  pictures to every thread's stack, and the motion SAD kernel (shared by
  `motion_cuda` and `motion_v2_cuda`) computes its vertical filter pass once
  per block. On an RTX 4090 with 3840x2160 8-bit frames the PSNR kernel drops
  from 1,750.5 to 17.7 us per frame, the motion SAD kernel from 136.5 to
  59.6 us and the moment kernel from 460.0 to 15.3 us (CUPTI, median of three
  traces); scores are unchanged. The whole-frame time barely moves, because
  copying each 4K frame to the device takes about 2.1 ms on that host's PCIe
  link, far longer than any of these kernels now runs.
