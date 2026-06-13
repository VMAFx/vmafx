### Added — SpEED-family GPU twins (HIP, SYCL) now wired

- New TU `core/src/feature/speed_internal.c` implements the 9-function
  contract declared in `speed_internal.h` (dimensions, float-stride,
  filter+downscale, covariance, eigendecomposition, QR factorisation,
  Q^T multiply, backward substitution, regularity check).  Shared
  between the CPU SpEED extractor and the GPU twins.
- `vmaf_fex_speed_chroma_hip`, `vmaf_fex_speed_temporal_hip`,
  `vmaf_fex_speed_chroma_sycl`, `vmaf_fex_speed_temporal_sycl` are now
  wired into meson and the extractor registry — looking them up via
  `vmaf_get_feature_extractor_by_name()` resolves.
- Two new CPU-vs-SYCL parity tests
  (`test_sycl_speed_chroma_parity`, `test_sycl_speed_temporal_parity`)
  pin the cross-backend tolerance at places=4 (ADR-0214).  Skip
  cleanly on hosts with no SYCL device.
- CUDA twins (`speed_chroma_cuda`, `speed_temporal_cuda`) remain
  unwired — the existing TUs reference `CHECK_CUDA` (helper is
  `CHECK_CUDA_RETURN`/`_GOTO`) and `CudaFunctions->cuMemAllocHost`
  (not a member); a repair pass lands in a follow-up.  Tracked as
  `T-CUDA-SPEED-TU-REPAIR-2026-05-31` in `docs/state.md`.

See [ADR-0964](docs/adr/0964-implement-speed-internal-and-wire-gpu-speed-extractors.md).
