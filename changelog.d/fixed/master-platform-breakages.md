- **Windows MinGW64**: Fixed `implicit declaration of function 'setenv'/'unsetenv'` in
  `test_gpu_dispatch_runtime.c` by adding `#ifdef _WIN32` portable wrappers that delegate
  to `_putenv_s` / `_putenv` on Windows (fixes CI job "Build — Windows MinGW64 (CPU)").

- **Windows MSVC + CUDA**: Fixed `syntax error: missing ')' before '*'` at
  `core/src/feature/iqa/ssim_simd.h:84` by adding `pthread_once_t`, `PTHREAD_ONCE_INIT`,
  and `pthread_once()` to `core/src/compat/win32/pthread.h`, mapping to the native Win32
  `INIT_ONCE` / `InitOnceExecuteOnce` API (fixes CI job "Build — Windows MSVC + CUDA").

- **Ubuntu CUDA**: Fixed `undefined reference to 'speed_score_ptx'` linker error by
  registering `cuda/speed/speed_score.cu` in `cuda_cu_sources` in `core/src/meson.build`
  so the PTX blob array is generated via the standard `bin2c` pipeline (fixes CI jobs
  "Build — Ubuntu CUDA" and "Build — Ubuntu CUDA Static").

- **macOS Metal**: Fixed `test_metal_kernel_coverage_audit` failure ("every Metal kernel
  basename must have a registered `<basename>_metal` extractor") by renaming the
  `integer_motion_v2_metal` extractor's `.name` field from the legacy `"motion_v2_metal"`
  to `"integer_motion_v2_metal"` and updating the dispatch table and all four affected
  tests consistently (fixes CI job "Build — macOS Metal (T8-1 scaffold)").
