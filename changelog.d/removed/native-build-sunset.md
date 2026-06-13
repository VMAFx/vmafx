# BREAKING: Sunset legacy native build modes (ADR-0728)

The following CI build configurations have been removed from
`.github/workflows/libvmaf-build-matrix.yml` and the required-checks list:

**Removed from CI matrix:**
- `Build — Windows MinGW64 (CPU)` — MinGW64 is not a VMAFX production target
- `Build — Ubuntu i686 gcc (CPU, no-asm)` — fork is 64-bit only
- `Build — Ubuntu gcc (CPU) + DNN` — superseded by Linux full-build in build.yml
- `Build — Ubuntu clang (CPU) + DNN` — superseded by Linux full-build in build.yml
- `Build — macOS clang (CPU) + DNN` — superseded by macOS leg in build.yml
- `Build — Ubuntu Vulkan (T5-1b runtime)` — folded into Linux full-build
- `Build — macOS Vulkan via MoltenVK (advisory)` — too fragile; no required gate
- `Build — Ubuntu HIP (T7-10b runtime)` — folded into Linux full-build
- `Build — macOS Metal (T8-1 scaffold)` — folded into macOS leg in build.yml
- `Build — Ubuntu gcc Static (CPU)` — pkgconfig verified within Linux full-build
- `Build — Ubuntu CUDA Static` — NVCC-static covered by Linux full-build
- `Build — Ubuntu SYCL` — folded into Linux full-build
- `Build — Ubuntu SYCL + CUDA` — folded into Linux full-build
- `Build — Windows MSVC + oneAPI SYCL (build only)` — SYCL in Linux full-build

**New canonical build matrix** (`build.yml`, ADR-0710):
- `Build — Linux (GCC, all backends)` — full stack: CUDA + SYCL + Vulkan + HIP + DNN + CPU
- `Build — macOS (Clang, CPU + Metal)` — Apple Clang + CPU + Metal scaffold
- `Build — Windows (MSVC + CUDA)` — MSVC + CPU + CUDA (build-only)

**Required-checks aggregator updated** to use new check names from `build.yml`
and `sanitizers.yml`. `Cppcheck (Whole Project)` removed (clang-tidy superset).

Implements ADR-0691 + ADR-0710. No functional change to the C library or CLI.
