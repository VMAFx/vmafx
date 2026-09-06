- `psnr_hvs_cuda` returned the luma-only score under the `psnr_hvs` name and
  omitted `psnr_hvs_cb` / `psnr_hvs_cr` entirely, because its `enable_chroma`
  option defaulted to `false` while the CPU and SYCL twins default it to `true`
  and the HIP twin computes chroma unconditionally. `psnr_hvs` is defined as
  `0.8*Y + 0.1*(Cb + Cr)`, so CUDA disagreed with CPU by ~4% on identical input
  — 41.4866616015 vs 41.7803055708 on a 960x540 pair — and
  `test_cuda_psnr_hvs_parity` had been failing on it by 4000x the tolerance.
  The default now matches every other backend. See
  [ADR-1203](docs/adr/1203-cuda-psnr-hvs-enable-chroma-default.md).
