### Fixed

- Correct two stale README badges: C++ standard badge now reads `C++23`
  (was `C++11`, reflecting the ADR-1003 / PR #608 bump); GPU capabilities
  badge now reads `CUDA · SYCL · HIP · Metal` (removes `Vulkan`, dropped per
  ADR-0726 / PR #607; adds `Metal`, Stage 1 live in `core/src/metal/`).
