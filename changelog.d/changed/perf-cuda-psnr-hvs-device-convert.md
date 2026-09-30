- **CUDA `psnr_hvs` reads the device pictures directly**
  (`T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29`, the CUDA port of ADR-1369).
  `psnr_hvs_cuda` no longer copies each frame to the host, converts it there and
  uploads float planes: the kernel reads the raw 8- to 12-bit samples, two threads
  per 8x8 block, one launch for every plane. Output is bit-identical to the previous
  twin at 8, 10 and 12 bits; 9- and 11-bit input, which it scored as -1.57 dB and NaN
  (`T-CUDA-PSNR-HVS-ODD-BPC-2026-09-30`), now matches the CPU. On an RTX 4090 a
  3840x2160 frame takes 3.20 ms instead of 18.28 ms (1920x1080: 0.43 instead of
  4.40). See
  [the CUDA backend guide](docs/backends/cuda/overview.md) and
  [the psnr_hvs page](docs/metrics/psnr-hvs.md#gpu-twins), which also explains why
  the CPU extractor differs from every GPU twin by up to 1.1e-2 dB at 3840x2160.
