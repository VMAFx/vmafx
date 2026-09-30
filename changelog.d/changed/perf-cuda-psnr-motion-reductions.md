- **`psnr_cuda` and the CUDA motion SAD kernel spend far less GPU time per
  frame (ADR-1392).** Both kernels added one atomic per warp to a single
  64-bit accumulator, which serialised them in the L2; they now add one per
  block. PSNR threads sum eight coalesced pixels each and select their plane
  without copying both pictures to their stack, and the motion SAD kernel
  (shared by `motion_cuda` and `motion_v2_cuda`) computes its vertical filter
  pass once per block. On an RTX 4090 with 3840x2160 8-bit frames the PSNR
  kernel drops from 1,750.5 to 17.7 us per frame and the motion SAD kernel
  from 136.5 to 59.6 us (CUPTI, median of three traces); scores are
  unchanged and still equal the CPU's. The whole-frame time barely moves,
  because copying each 4K frame to the device takes about 2.1 ms on that
  host's PCIe link, far longer than either kernel now runs.
