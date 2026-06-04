### Fixed

- **core/ref**: Use `atomic_fetch_sub_explicit(..., memory_order_acq_rel)` in
  both `ref.c` and `ref.cpp` for the reference-count decrement. This makes the
  last-decrementer acquire ordering explicit, ensuring the destructor path sees
  all prior writes from other reference holders (fixes r5-memory-ordering finding 1;
  ADR-1020).

- **core/feature_collector**: Add a `destroyed` flag to `VmafFeatureCollector`
  set under the mutex before the final unlock in `vmaf_feature_collector_destroy`.
  All five public entry points that acquire the lock now test the flag immediately
  after locking and return `-ENODEV` if set, closing the mutex-destroy-after-unlock
  race where a concurrent locker could acquire a destroyed mutex (fixes
  r5-memory-ordering finding 2; ADR-1020).

- **core/picture_pool**: Copy `pool->pictures[idx]` to a stack-local while still
  holding the pool lock in `vmaf_picture_pool_fetch`, then unlock before assigning
  to the caller's `*pic`. This prevents a use-after-free if `vmaf_picture_pool_close`
  frees the pictures array in the window between unlock and the slot read (fixes
  r5-memory-ordering finding 3; ADR-1020).
