- **SYCL and HIP `psnr_hvs` score 4:0:0 input, and the HIP twin takes
  `enable_chroma`** (`T-SYCL-HIP-PSNR-HVS-YUV400-REFUSED-2026-10-01`).
  `psnr_hvs_sycl` and `psnr_hvs_hip` refused 4:0:0 pictures at `init()`
  (`YUV400P unsupported`), where the CPU extractor and `psnr_hvs_cuda` score the
  luma plane and emit `psnr_hvs_y` and `psnr_hvs`. Both twins now do the same.
  `psnr_hvs_hip` also gains the CPU extractor's `enable_chroma` option (default
  `true`): with `false` it uploads, dispatches and scores luma only, like the
  CUDA and SYCL twins. Scores for 4:0:0 and for `enable_chroma=false` are
  identical to the CPU's at `--precision max`, measured on an Arc A380, a
  gfx1036 and an RTX 4090. See
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#sample-conversion).
