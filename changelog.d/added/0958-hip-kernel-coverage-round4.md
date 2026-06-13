## Added

- **HIP kernel parity-test coverage round 4** (`core/test/`): adds 2
  new parity tests closing the next batch of reachable HIP-vs-CPU gaps
  after PR #443 / ADR-0945 — `test_hip_ssimulacra2_parity` and
  `test_hip_float_ssim_parity` (ADR-0958). Each test follows the
  round-1/2/3 template: synthetic 256x144 YUV420P fixture, CPU
  reference vs. HIP score, skip cleanly with `[skip: no HIP device]`
  or `[skip: HIP scaffold ENOSYS]` on hosts lacking an AMD GPU /
  `enable_hipcc=true`. Tolerance places=3 (1e-3) for both —
  multi-scale Gaussian pyramid (ssimulacra2) and windowed SSIM
  pooling (float_ssim) sit at the MS-SSIM rounding budget. Lifts HIP
  backend parity coverage from 13/17 → 15/17 extractors (76% → 88%).
  `speed_chroma_hip` / `speed_temporal_hip` deferred to a follow-up
  PR — round-4 verification surfaced a pre-existing latent link
  defect in the speed-family GPU twins
  (`speed_internal_init_dimensions` / `speed_internal_float_stride`
  declared in `core/src/feature/speed_internal.h` but never defined),
  tracked as T-HIP-SPEED-INTERNAL-IMPL-MISSING-2026-05-31.
  `float_moment_hip` remains deferred pending the CPU/HIP
  `provided_features` mismatch resolution.
