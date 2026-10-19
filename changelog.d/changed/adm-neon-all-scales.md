- **Integer ADM runs on NEON at every scale on aarch64 (Netflix/vmaf
  `8bc5a5c6a`, `b41d2340a`).** The contrast masking of scale 0 and of scales
  1 to 3, the DWT of scales 1 to 3 and the decouple of scales 1 to 3 (at an
  enhancement gain limit of 1) have NEON kernels. They return the scalar
  kernels' bits: scores are byte-identical at every `--cpumask` setting
  ([Arm backend](docs/backends/arm/overview.md)).
