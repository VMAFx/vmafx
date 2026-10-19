- **Host fences wait on a condition variable, and the MSVC builds have a timed
  wait.** `vmafx_fence_wait()` on a host fence, and `vmafx_window_wait()`,
  sleep until the fence is signalled instead of polling every 50 µs (1 ms on
  Windows); on Linux the deadline is kept on `CLOCK_MONOTONIC`. The Win32
  pthread shim of the MSVC, clang-cl and icx-cl builds gains
  `pthread_cond_timedwait()`, so `test_thread_pool_backpressure` now builds and
  runs on the MSVC lanes too
  ([Threads on MSVC](docs/getting-started/building-on-windows.md#threads-on-msvc)).
