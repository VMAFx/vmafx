## Added

- **HIP kernel parity-test coverage round 5** (`core/test/`): adds 2
  new parity tests closing the speed-family HIP-vs-CPU gaps deferred
  from round 4 (ADR-0958) — `test_hip_speed_chroma_parity` and
  `test_hip_speed_temporal_parity` (ADR-1004). Both tests follow the
  round-1/2/3/4 template: synthetic 768x432 YUV420P fixture (matching
  the CUDA speed-family gates for cross-backend comparability), CPU
  reference vs. HIP score, skip cleanly with `[skip: no HIP device]`
  or `[skip: HIP scaffold ENOSYS]` on hosts lacking an AMD GPU /
  `enable_hipcc=true`. Tolerance places=4 (1e-4) — SpEED's QR /
  eigensolver runs on CPU for both backends. The round-4 link-defect
  blocker (`speed_internal_init_dimensions` / `speed_internal_float_stride`
  missing `.c` implementation) was resolved by ADR-0964 / PR #465.
  Lifts HIP backend parity coverage from 15/17 → 17/17 extractors
  (88% → 100%) for all non-deferred kernels.
