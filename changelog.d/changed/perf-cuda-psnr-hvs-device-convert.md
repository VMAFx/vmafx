- **CUDA `psnr_hvs` converts samples on the device without host roundtrip
  (porting ADR-1369, closing T-CUDA-PSNR-HVS-HOST-ROUNDTRIP-2026-09-29).**
  `psnr_hvs_cuda` previously downloaded each plane to pinned host uint memory,
  ran a CPU float-conversion loop, and re-uploaded 4-byte float planes back to
  the device per frame. The ported extractor directly reads pitched integer
  samples on-device across all bit depths (8, 9, 10, 11, 12, 16 bpc, also
  fixing odd-depth scaling on 9-bit and 11-bit inputs), executes 2 cooperating
  threads per 8x8 block with warp shuffle exchange (`__shfl_xor_sync`) and
  in-place 8x8 DCT in shared memory, and launches all planes in a single grid
  into one partials buffer read back asynchronously via `VmafCudaKernelReadback`.
  On an RTX 4090, 4K (3840x2160) frame time drops from 17.58 ms (56.9 fps) to
  3.00 ms (333.2 fps), a 5.86x throughput improvement (faster than 16 CPU threads
  at 5.17 ms). All scores are bit-identical to the pre-port CUDA twin (max
  absolute difference 0.0). See
  [the CUDA backend guide](docs/backends/cuda/overview.md#psnr_hvs_cuda-on-device-conversion-and-cooperative-dct).
