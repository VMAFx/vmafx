- **`integer_ssim_cuda` returns the CPU's `ssim` bit for bit.** The CUDA twin
  of the fixed-point `ssim` extractor computed the CPU's moments and the
  CPU's per-pixel term, but added the terms per 16x8 block where the CPU adds
  them one after the other across the whole frame. Its score matched the CPU
  on no measured frame and was up to 1.1e-11 away (3.6e-10 with
  `enable_db`); on very small identical frames it could report `+inf` where
  the CPU reports about 156 dB. The twin now reads the per-pixel terms back
  and adds them on the host in the CPU's order
  ([ADR-1424](docs/adr/1424-cuda-ssim-cpu-frame-sum.md)). Measured on an RTX
  4090 at `--precision max`: identical on every frame of the Netflix pair at
  8, 10, 12 and 16 bits, both 1080p checkerboard pairs and BBB 3840x2160,
  with and without `enable_db` / `clip_db`, and on frames from 1x1 up. The
  parity gate now knows the `ssim` feature and compares the CUDA twin with
  tolerance 0. The price is time: a 3840x2160 frame takes 9.7 ms instead of
  2.2 ms, because 8.3 million terms are read back and added sequentially; at
  576x324 the difference is not measurable. The twin also needs 66 MB more
  device memory and as much pinned host memory at 3840x2160. The SYCL, HIP
  (above 64x64) and Metal twins still agree with the CPU to four decimal
  places or better.
