---
paths:
  - core/src/compat/win32/pthread.h
  - core/src/compat/win32/pthread_timeout.h
  - core/test/test_win32_pthread_timeout.c
  - core/test/test_win32_pthread_shim_contract.py
invariant: MSVC threads = header-only Win32 shim, no pthread-win32; timed wait ETIMEDOUT only past deadline.
---
<!-- markdownlint-disable MD013 MD032 MD060 -->
# Win32 pthread shim (MSVC, clang-cl, icx-cl)

- One threads implementation on MSVC: `compat/win32/pthread.h`, header-only,
  over SRWLOCK / CONDITION_VARIABLE / INIT_ONCE / `_beginthreadex`, wired by
  `core/meson.build` when `pthread.h` is absent. MinGW-w64 uses its own
  winpthreads. Upstream's bundled pthread-win32 (Netflix/vmaf `bcd6e6159`,
  `-Dbundled_winpthreads`) is not taken (ADR in `docs/adr/`, Q-302): no
  second implementation, no LGPL static link, no CMake subproject.
- Surface = what the tree calls, nothing more:
  `core/test/test_win32_pthread_shim_contract.py` fails on a call to a
  function the shim lacks. New pthread call in code that builds on Windows
  -> add it to the shim in same PR.
- `pthread_cond_timedwait()`: `abstime` absolute on CLOCK_REALTIME (default
  clock; shim has no condattr), converted against `timespec_get(TIME_UTC)` by
  `vmaf_w32_timeout_ms()` (`pthread_timeout.h`: rounded up, capped at
  `INFINITE - 1`, overflow-safe). Wake before deadline (spurious or capped)
  -> 0, caller loops. `ETIMEDOUT` only once the clock is past `abstime`.
  Bad arguments / nsec out of range -> `EINVAL`. Arithmetic tested on every
  host (`test_win32_pthread_timeout`); call itself on MSVC CI legs
  (`test_thread_pool_backpressure`, `test_vmafx_host_fence_wait`).
- Host fences (`core/src/vmafx/fence.c`) wait on this timed wait on MSVC;
  never go back to polling there.
