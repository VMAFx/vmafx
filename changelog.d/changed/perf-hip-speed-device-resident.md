- **The HIP SpEED twins run entirely on the device in the CPU's fp32
  arithmetic (ADR-1384).** `speed_chroma_hip` and `speed_temporal_hip` no
  longer copy and filter planes on the host or read the 25x25 covariance back
  for the eigenvalues and the QR solve: each frame is one staged upload, eight
  kernels (the ADR-1358 chain) and one result read, with `collect()` the only
  wait. The kernels are built without FMA contraction and with correctly
  rounded division and square root, so every per-frame score equals the CPU
  extractor's when the CPU's `log2f` is correctly rounded; against a glibc
  build, whose `log2f` misrounds about 0.4 % of arguments, a few chroma frames
  differ in the last float bits. The init-time setup is now one routine,
  `speed_internal_gpu_configure()`, shared with the SYCL twins. Request the
  twins by name (`--feature speed_chroma_hip`). Not yet run on an AMD device:
  see `docs/state.md` (`T-HIP-SPEED-HOST-RESIDUAL-2026-09-29`) and
  [SpEED](docs/metrics/speed_qa.md#hip-device-resident-cpu-fp32-arithmetic).
