- **dev/Containerfile**: add `libclang-rt-19-dev` to the build-deps apt layer so
  that `b_sanitize=address,undefined` meson builds link correctly against the
  clang-19 ASan/UBSan runtime libraries (previously the linker failed with
  `cannot find libclang_rt.asan.a`).
- **vmaf-tune `--score-backend`**: remove `vulkan` from `ALL_BACKENDS` /
  `DEFAULT_FALLBACKS` in `score_backend.py` and from all argparse `choices=`
  tuples, following the Vulkan backend removal in ADR-0726. Requesting
  `--score-backend vulkan` now raises a `ValueError` at startup.
- **vmaf-tune docs**: update `docs/usage/vmaf-tune-score-backend.md` to reflect
  the `cuda → sycl → hip → cpu` fallback chain and remove the Vulkan row from
  the accepted-values table.
- **Go CI (`.github/workflows/go-ci.yml`)**: post-ADR-0700 sweep miss — the
  cgo-link build step ran `meson setup core/build-cpu` from repo root, which
  fails with `Neither source directory 'core/build-cpu' nor build directory
  None contain a build file meson.build.` since the rename moved `meson.build`
  into `core/`. Pass the source dir explicitly (`meson setup core/build-cpu
  core ...`). Restores Go CI green on master.
- **SYCL integer extractors (`core/src/feature/sycl/integer_adm_sycl.cpp`,
  `integer_vif_sycl.cpp`)**: add the missing `close_fex_sycl` forward
  declaration. The init-failure cleanup paths added by ADR-0784 (sycl-init-
  failure-cleanup-leaks) call `close_fex_sycl(fex)` from within
  `init_fex_sycl`, but the function is defined later in the file as
  `static int` without a forward decl, so SYCL builds fail with
  `error: use of undeclared identifier 'close_fex_sycl'`. Matches the pattern
  already used by `float_*_sycl.cpp`. Restores Linux GCC (all backends),
  Ubuntu SYCL, Ubuntu SYCL + CUDA, and macOS SYCL builds.
- **CUDA SpEED kernel embed (`core/src/meson.build`)**: register
  `core/src/feature/cuda/speed/speed_score.cu` in the `cuda_cu_sources`
  dict so it is compiled to PTX and embedded as the `speed_score_ptx`
  C string. Both `speed_chroma_cuda.c` and `speed_temporal_cuda.c`
  declare `extern const char speed_score_ptx[]` and call
  `cuModuleLoadData(&module, speed_score_ptx, ...)`, but the kernel
  source was orphaned in the meson dict (added by ADR-0567 but never
  wired). Without this entry the link fails with `undefined reference
  to 'speed_score_ptx'`. Restores Docker Image Build, Linux GCC (all
  backends), Ubuntu CUDA, Ubuntu CUDA Static, Ubuntu SYCL + CUDA, and
  Windows MSVC + CUDA build legs.
- **Windows MinGW64 test (`core/test/test_gpu_dispatch_runtime.c`)**: map
  POSIX `setenv` / `unsetenv` to `_putenv_s` on `_WIN32 && !__CYGWIN__`.
  MinGW's libc doesn't expose POSIX env routines so the build failed with
  `implicit declaration of function 'setenv'`. Restores the Windows
  MinGW64 (CPU) leg.
- **Metal kernel coverage audit (`core/test/test_metal_kernel_coverage_audit.c`)**:
  update the kernel-basename list to use the *registered extractor name*
  (`motion_v2`, matching `integer_motion_v2_metal.mm`'s actual `.name =
  "motion_v2_metal"`), not the `.mm` filename. The audit asserted
  `integer_motion_v2_metal` existed and failed because that name is
  never registered — the established name is the short form, consistent
  with `dispatch_strategy.c`, `test_metal_kernel_registration.c`,
  `test_metal_motion_v2_parity.c`, and `test_metal_smoke.c`. Restores
  the macOS Clang (CPU + Metal) and Metal (T8-1 scaffold) test legs.
- **Windows MSVC pthread_once (`core/src/compat/win32/pthread.h`)**:
  add `pthread_once_t` / `PTHREAD_ONCE_INIT` / `pthread_once()` to the
  Win32 pthread shim. `iqa/ssim_simd.h` declares
  `iqa_ssim_install_dispatch_once(pthread_once_t *guard, void (*installer)(void))`
  and `float_ssim.c` / `float_ms_ssim.c` use a static
  `pthread_once_t s_dispatch_guard = PTHREAD_ONCE_INIT` for the SIMD
  dispatch install (TSan race audit 2026-05-30). MSVC fails the whole
  Windows + CUDA leg because the existing win32 shim only provides
  mutex / cond / thread, not once-init. Backs onto
  `InitOnceExecuteOnce` (Windows Vista+, which is the documented
  floor for this shim). Restores the MSVC + CUDA and MSVC + oneAPI SYCL
  build-only legs.
- **ASan/TSan: `test_y4m_alloc_failure` (`core/test/test_y4m_alloc_failure.c`)**:
  skip under sanitizer builds. The test caps virtual address space via
  `setrlimit(RLIMIT_AS, 256 MiB)` to force the y4m parser's `malloc` to
  fail; AddressSanitizer / ThreadSanitizer / MemorySanitizer reserve a
  multi-TB shadow virtual region at startup, so any subsequent ASan
  internal `mmap` blows past the 256 MiB cap and the test process
  aborts with "ERROR: Failed to mmap" before the parser even runs. The
  guarded regression (dst_buf-NULL bug) is C-level, not sanitizer-
  surfaceable, so skipping under sanitizers gives up no coverage.
  Restores the `Sanitizers — ASan + UBSan (PR gate)` and `ASan + UBSan
  + MSan (address / thread)` legs.
