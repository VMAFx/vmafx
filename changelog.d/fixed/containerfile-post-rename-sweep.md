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
- **MSVC C17 `ATOMIC_VAR_INIT` (`core/src/feature/iqa/ssim_tools.c`)**:
  use direct value initialisation instead of `ATOMIC_VAR_INIT(NULL)`. The
  macro was deprecated in C17 and MSVC's `<stdatomic.h>` does not provide
  it (`error C2099: initializer is not a constant`). Direct
  initialisation `static _Atomic(...) x = NULL` has been semantically
  equivalent to `ATOMIC_VAR_INIT(NULL)` on every conforming C11
  implementation. Unblocks the MSVC + CUDA and MSVC + oneAPI SYCL builds.
- **MinGW64 + MSVC `mkdtemp` (`core/test/test_mkdirp.c`)**: replace
  the POSIX-only `mkdtemp` call with a portable `vmaf_mkdtemp_portable`
  helper that picks a temp root from `TMPDIR`/`TEMP`, generates a unique
  suffix from pid+time+counter, and creates the directory with `mkdir`
  (POSIX) or `_mkdir` (MSVCRT). MinGW64's libc has `mkdtemp` but the
  call returns NULL when `/tmp/` isn't where the runtime expects;
  MSVCRT lacks the function entirely. Restores `test_mkdirp` on the
  Windows MinGW64 (CPU) leg.
- **Tiny AI `iter_frames` test fixture (`ai/tests/test_frame_loader.py`)**:
  `_popen_factory`'s `fake_popen` now accepts the `stderr` keyword that
  `iter_frames` passes in production (captured for ffmpeg diagnostics
  surfacing). Pre-fix every `iter_frames(..., popen=_popen_factory(...))`
  test failed with `TypeError: fake_popen() got an unexpected keyword
  argument 'stderr'`.
- **Tiny AI parquet rollback test (`ai/tests/test_parquet_utils.py`)**:
  monkey-patch `parquet_utils._write_v2` to raise instead of subclass-
  overriding `DataFrame.to_parquet`. The v2 write path goes through
  `pq.write_table`, not the DataFrame method, so the old subclass mock
  was a no-op and `pytest.raises(RuntimeError)` never tripped (`DID NOT
  RAISE`). The rollback semantics being tested (temp file cleanup on
  serialiser exception) are now properly exercised.
- **MCP smoke `_run_benchmark` test (`mcp-server/vmaf-mcp/tests/test_probe_findings_2026_05_17.py`)**:
  align `test_bug3_run_benchmark_surfaces_silent_pipefail` with ADR-0608 E-1
  contract (raise `RuntimeError` so MCP marks `isError=True`); test was
  still asserting the pre-ADR-0608 dict-with-`error` shape.
- **Tiny AI `_FakeProcess.wait()` (`ai/tests/test_frame_loader.py`)**: accept
  the `timeout` keyword that `iter_frames` passes per the ADR-0608 progress
  watchdog.
- **Tiny AI manifest tests (`ai/tests/test_data_datasets_branches.py`)**:
  use deterministic 64-char hex digests; pydantic validator on
  `ManifestEntry.sha256` now rejects the old 8-char shorthand.
- **Clang-Tidy NOLINTs**: inline justifications on three findings on files
  touched by this PR — `concurrency-mt-unsafe` (getenv in single-threaded
  test), `bugprone-reserved-identifier` (`_POSIX_C_SOURCE` feature-test
  macro), `readability-non-const-parameter` (POSIX pthread_once_t guard).
