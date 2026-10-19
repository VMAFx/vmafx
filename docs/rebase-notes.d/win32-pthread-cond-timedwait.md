## Win32 pthread shim: timed wait; host fences on a condition variable (2026-10-08)

- `core/src/compat/win32/pthread.h` gains `pthread_cond_timedwait()` over
  `SleepConditionVariableSRW()`. Its deadline arithmetic is
  `core/src/compat/win32/pthread_timeout.h` (`vmaf_w32_timeout_ms()`), tested
  on every host by `test_win32_pthread_timeout`. Upstream's bundled
  pthread-win32 (Netflix/vmaf `bcd6e6159`, `-Dbundled_winpthreads`, and the
  pthread parts of `a2660554e`, `8618ba5cd`, `5079124ea`, `e2f8b24ab`) is not
  taken (Q-302). **On sync**: do not add the `libvmaf/subprojects/pthread-win32`
  submodule, the CMake subproject or the option.
- `core/src/vmafx/fence.c`: a host fence carries a mutex and a condition
  variable (`CLOCK_MONOTONIC` on Linux); `vmafx_host_fence_signal()`
  broadcasts, `vmafx_host_fence_wait()` (now non-const) waits in chunks of at
  most 1 s against the monotonic deadline. The virtual test clock keeps the
  poll path. Backend lanes that add fence kinds keep `vmafx_fence_poll()`.
- `core/test/test_thread_pool_backpressure.c` reads `timespec_get(TIME_UTC)`
  on MSVC; the Meson probe `has_cond_timedwait` now finds the shim's function,
  so the test builds on the MSVC lanes.
- `core/test/test_win32_pthread_shim_contract.py`: `PROBED` is empty; Linux-only
  `pthread_condattr_*` calls in `fence.c` are allowed inside their guard
  (`PLATFORM_ONLY`).
