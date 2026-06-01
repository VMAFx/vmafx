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

- **Windows MSVC + CUDA (Layer-2)**: Fixed `error C2099: initializer is not a constant`
  at `core/src/feature/iqa/ssim_tools.c:95` by replacing the deprecated
  `ATOMIC_VAR_INIT(NULL)` initialiser with plain `NULL` — `ATOMIC_VAR_INIT` is absent from
  MSVC's `<stdatomic.h>` and deprecated in C17; `NULL` is semantically identical per
  C11 §7.17.2.1 p3.

- **Windows MSVC + SYCL (Layer-2)**: Fixed `fatal error: 'unistd.h' file not found` at
  `core/test/test_svm_api.c:47` by guarding the include with `#ifndef _WIN32 / #endif`
  and pulling in `<windows.h>` + `<io.h>` in its place.

- **Windows MinGW64 (Layer-2)**: Fixed two runtime test failures (`test_svm_api::test_save_load_roundtrip`
  and `test_mkdirp::test_mkdirp_single_level`) caused by `mkstemp`/`mkdtemp` against
  hardcoded `/tmp/...` paths — MSYS2/MinGW64 in GitHub Actions does not expose a usable
  `/tmp` from the MINGW64 shell.  Added portable `make_svm_temp_path()` (GetTempPathA on
  `_WIN32`, mkstemp on POSIX) in `test_svm_api.c` and `portable_mkdtemp()` +
  MKDTEMP/RMDIR macros in `test_mkdirp.c`, following the pattern documented in
  `core/test/AGENTS.md §6`.
