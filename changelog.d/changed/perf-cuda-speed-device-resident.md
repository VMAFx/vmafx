- **The CUDA SpEED twins run entirely on the device (ADR-1380).**
  `speed_chroma_cuda` and `speed_temporal_cuda` no longer copy their planes to
  the host to filter them or solve the 25x25 eigenvalue problem and QR system
  there: the whole chain runs on the GPU, fed by device-to-device copies of the
  planes the engine already uploaded, with one 40-byte readback and one wait
  per frame. Every rounding the CPU performs is spelled with a round-to-nearest
  intrinsic, so the scores move from within 1e-4 of `--backend cpu` to equal to
  it, against a CPU build that rounds `log2f` correctly and does not fuse
  multiply-adds (an icx build without `-march=native`; a gcc build on glibc
  differs in the last bits on a few frames). The SpEED page now also states
  that the SYCL and CUDA `lanczos4` prescale is up to 4.4e-4 relative from the
  CPU rather than within tolerance. Checked frame by frame through a host
  emulation of the CUDA driver; not yet run or timed on an NVIDIA GPU. See
  [SpEED](docs/metrics/speed_qa.md#cuda-the-same-chain-on-the-device).
