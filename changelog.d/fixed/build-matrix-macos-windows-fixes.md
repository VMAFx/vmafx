- Fix NEON `neon_any_nonzero_s32` uint64-truncation bug that incorrectly
  skipped rows with alternating zero/non-zero y_row values on arm64,
  causing the checkerboard motion score to return 0.0 instead of 12.55
  on macOS arm64 CI runners.
- Fix Python harness `disable_avx` emitting `--cpumask -1` which the
  CLI (ADR-1088) now rejects; updated to `4294967295` (0xFFFFFFFF).
- Fix Windows MSVC/CUDA/SYCL builds: add `pthread_dependency` to
  `picture_pool_cpp23_lib` and `gpu_picture_pool_cpp23_lib` so the
  win32 pthreads shim include path is resolved.
- Remove stale Vulkan matrix rows (`Build — Ubuntu Vulkan` and
  `Build — macOS Vulkan via MoltenVK`) from the CI workflow; the Vulkan
  backend was removed in ADR-0726 and `enable_vulkan` is no longer a
  valid meson option.
- Update `test_run_preserves_user_env` to expect `LC_ALL=C` and
  `LANG=C` that `ProcessRunner` unconditionally stamps for deterministic
  subprocess error messages.
