- SpEED-chroma on the CUDA backend returned exactly `0.000000` for
  `speed_chroma_u` / `_v` / `_uv` at 4K and above, pulling the default
  model's pooled VMAF 3.42 points off the CPU score (63.733364 vs
  67.150063 on a 3840x2160 pair) while the run still exited 0.
  Two defects: the backward-substitution launch derived its block size
  from the *block count*, so any picture with more than 256 linear
  systems exceeded CUDA's 1024-thread block limit and the kernel never
  ran; and all three GPU twins (CUDA, SYCL, HIP) conflated "singular
  covariance matrix" with "hard device failure", so the launch error
  was routed into the CPU twin's singular-matrix imputation and, with
  both chroma channels failing, averaged to `0.0` and reported success.
  Singularity is now reported separately from failure, device errors
  fail the frame, and the twins adopt the CPU rule that a channel with
  exactly one singular side scores 0. CPU, CUDA and SYCL now agree at
  4K. See [ADR-1202](docs/adr/1202-cuda-speed-chroma-4k-launch-bounds.md).
