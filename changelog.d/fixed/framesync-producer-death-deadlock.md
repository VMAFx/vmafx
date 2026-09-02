- **`vmaf_framesync_retrieve_filled_data` hangs forever when the producer
  thread dies without calling `vmaf_framesync_submit_filled_data`.**
  The consumer's `pthread_cond_wait` loop had no exit path on producer
  failure, causing `vmaf_thread_pool_wait` (and therefore `vmaf_close`) to
  block indefinitely.  Additionally, calling `vmaf_framesync_destroy` while
  a consumer was still in `pthread_cond_wait` was undefined behaviour per
  POSIX (`pthread_cond_destroy` with waiters: UB).

  Fix: add an `aborted` flag to `VmafFrameSyncContext` and a new
  `vmaf_framesync_abort()` function that sets the flag and broadcasts on
  the condvar.  `retrieve_filled_data` now checks the flag before entering
  `cond_wait` and immediately after waking, returning `-ECANCELED` on both
  paths.  `vmaf_framesync_destroy` calls `vmaf_framesync_abort` as a
  defence-in-depth safety net before destroying the condvar.
  (ADR-1092, `core/src/framesync.c`)
