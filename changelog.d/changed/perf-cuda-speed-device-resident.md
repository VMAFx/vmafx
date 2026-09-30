- **The CUDA SpEED twins run entirely on the device (ADR-1380).**
  `speed_chroma_cuda` and `speed_temporal_cuda` no longer copy their planes to
  the host to filter them or solve the 25x25 eigenvalue problem and QR system
  there: the whole chain runs on the GPU, fed by device-to-device copies of the
  planes the engine already uploaded, with one 40-byte readback and one wait
  per frame. Every rounding the CPU performs is spelled with a round-to-nearest
  intrinsic, so the scores move from within 1e-4 of `--backend cpu` to equal to
  it, against a CPU build that rounds `log2f` correctly and does not fuse
  multiply-adds (an icx build without `-march=native`; a gcc build on glibc
  differs in the last bits on a few frames). On an RTX 4090 every frame of the
  Netflix 576x324 pair and of BBB 3840x2160 is identical, a 4K
  `speed_chroma_cuda` frame takes 6.89 ms instead of 24.90 ms, and
  `speed_temporal_cuda`, which failed at 1920x1080 and above with
  `CUDA_ERROR_INVALID_VALUE`, now runs 4K at 5.88 ms per frame. The `lanczos4`
  prescale of the SYCL and CUDA twins is not exact: up to 5.7e-4 relative from
  the CPU on a smooth 1080p gradient on an RTX 4090. See
  [SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device).
