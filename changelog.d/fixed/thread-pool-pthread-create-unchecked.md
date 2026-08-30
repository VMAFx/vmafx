- `vmaf_thread_pool_create` now checks the return value of `pthread_create`
  and handles partial-success (at least one thread started) and total-failure
  (zero threads started, returns `-EAGAIN`/`-EPERM` to the caller) gracefully.
  Previously, a failed `pthread_create` left `n_threads` counting non-existent
  workers, causing `vmaf_thread_pool_wait` to hang indefinitely on teardown
  under process-limit pressure (`ulimit -u`, container thread caps, etc.).
  Also fixes a data race in `vmaf_thread_pool_destroy`: the worker count
  used to iterate `thread_data_free` is now read from a new immutable
  `n_workers_created` field instead of the mutable `n_threads` counter that
  runner threads decrement under the lock.
