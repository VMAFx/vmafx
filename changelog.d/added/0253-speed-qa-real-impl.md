- **`speed_qa` real SpEED-QA implementation** (`core/src/feature/speed_qa.c`):
  replaces the 0.0-placeholder scaffold with a working spatial and temporal
  entropic-differencing extractor per Bampis et al. 2017 (IEEE SPL 24(9)).
  Algorithm: 7x7 Gaussian-windowed local variance per block;
  per-block entropy H = 0.5 * log2(2*pi*e*(sigma^2+1)); spatial score =
  mean(H) over distorted luma; temporal score = mean(H) over frame-difference;
  output = spatial + temporal. Self-contained (no float dependency). ADR-0253.
- **`core/test/test_speed_qa.c`**: five smoke tests (registration, vtable,
  flat/noise entropy ordering, temporal positivity). All 53 libvmaf tests pass.
- **`docs/metrics/speed_qa.md`**: user-facing documentation for the extractor.
