- Enforced reproducible GPU clang-tidy ratchet configuration by adding
  `scripts/ci/check-tidy-build-dir.py` to `make tidy-ratchet` and `make tidy-ratchet-write`.
  GPU lanes (`cuda`, `hip`, `sycl`) now fail closed if the build directory is in-repo or
  configured without `-Db_lto=false`, preventing clang LTO flag rejection and generated header
  pollution. Re-recorded `scripts/ci/tidy-baseline-hip.json` (360 TUs, 1310 warnings, including 12 MEX sources) and reconciled baseline measurements against the documented reproducible configuration (ADR-1323).
