- **Nine HIP test files conform to clang-tidy and HISS standards (part 1).**
  The first batch of HIP test sources (`test_hip_smoke.c`,
  `test_hip_float_adm_parity.c`, `test_hip_motion3_parity.c`,
  `test_hip_float_vif_parity.c`, `test_hip_float_moment_parity.c`,
  `test_hip_cambi_parity.c`, `test_hip_ssimulacra2_parity.c`,
  `test_hip_motion_v2_parity.c`, `test_hip_float_psnr_parity.c`) were brought to
  zero clang-tidy findings in the HIP lane (-245 baseline findings) under
  [ADR-1142](docs/adr/1142-clang-tidy-debt-ratchet.md). Functions exceeding
  branch and statement thresholds were split into clean helpers satisfying
  HISS-04 / NASA JPL Rule 4. C23 `nullptr` diagnostics are scoped under ADR-1138
  to maintain MSVC C portability, and `__HIP_PLATFORM_AMD__` is supplied by the
  build harness (ADR-1263). All tests pass on AMD gfx1036.
