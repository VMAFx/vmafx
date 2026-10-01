- **CUDA `psnr_hvs` returns the CPU extractor's scores bit for bit**
  (`T-PSNR-HVS-CPU-FLOAT-SUM-4K-2026-09-30`,
  [ADR-1397](docs/adr/1397-psnr-hvs-twins-cpu-float-sum.md)). The CPU adds every
  masked coefficient error of a plane into one running `float`, so its score
  depends on the order of the additions; `psnr_hvs_cuda` summed each block first
  and was up to 1.7e-2 dB from `--backend cpu` at 3840x2160, beyond the parity
  tolerance. The kernel now stores the 64 terms of every block in the CPU's
  arithmetic and the host adds them in the CPU's order: `psnr_hvs`, `psnr_hvs_y`,
  `psnr_hvs_cb` and `psnr_hvs_cr` are identical to the CPU at `--precision max`
  from 576x324 to 3840x2160 and at 8 to 12 bits, and the parity gate compares
  this twin with tolerance 0. `psnr_hvs_cuda` scores therefore change in their
  last digits (by up to 1.7e-2 dB at 3840x2160). The twin is slower for it: on
  an RTX 4090 a 3840x2160 frame takes 12.2 ms instead of 2.4 ms, and the term
  buffer needs 65 MB per 3840x2160 frame; tuning is tracked as
  `T-CUDA-PSNR-HVS-EXACT-SUM-THROUGHPUT-2026-10-01`. The HIP and SYCL twins
  keep their per-block sums until their rewrites land. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#agreement-with-the-cpu-extractor).
