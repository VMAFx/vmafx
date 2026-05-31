<!-- markdownlint-disable MD013 -->
# Research-0097: `vmaf_thread_pool_create` — unchecked `pthread_create` and racy `n_threads` read in `destroy`

**Date**: 2026-05-10
**Branch**: `fix/thread-pool-pthread-create-unchecked`
**Found by**: Round-9 angle-5 (resource-limit graceful handling audit)
**Fixed in**: (this PR)

## Summary

Two related defects in `core/src/thread_pool.c` (fork-local additions
made when the inline-data job pool and the `n_workers_created` separation
were added):

1. **`pthread_create` return value unchecked** (CWE-252): if thread creation
   fails with `EAGAIN` (process-limit exhaustion, e.g. `ulimit -u` or container
   cgroup limit), `p->n_threads` stays at `cfg.n_threads` even though fewer
   worker threads actually started. `vmaf_thread_pool_wait()` then enters its
   stop-path branch (`while (pool->stop && pool->n_threads)`) and waits for
   `n_threads` to reach 0 via runner-thread exit signals. Because the
   non-started threads never signal, the wait never completes: **process hangs
   forever on destruction / `vmaf_close()`**.

2. **`n_threads` read without the mutex in `destroy`** (data race): line
   `const unsigned n_workers = pool->n_threads` (former line 277) reads
   `n_threads` without holding `pool->queue.lock`. Runner threads decrement
   `n_threads` under the lock as they exit. On a lightly-loaded system where
   workers exit before `destroy` acquires the lock, this is a benign read of
   an already-stable 0. On a busy system or under TSan, this is a detected
   data race (read/write on different threads without synchronisation).

## Root cause

Both bugs were pre-existing in the upstream Netflix codebase; the fork
extended `thread_pool.c` with the inline-data job pool but did not add a
`n_workers_created` field or check `pthread_create` return values at that
time. The extend touched these lines and inherited the latent defects.

## Impact

- **Bug 1 (pthread_create)**: process hangs under resource pressure.
  Trigger: `ulimit -u <N>` where N is close to the current thread count,
  then run `vmaf --threads 8 …`. Expected: graceful `-EAGAIN` propagated
  to `vmaf_close` caller. Actual: infinite wait.
- **Bug 2 (racy n_workers read)**: benign in practice (the read value is
  used only to bound the `thread_data_free` loop after `vmaf_thread_pool_wait`
  already returned — so all workers have exited). However, it is a
  detectable data race under TSan and constitutes UB under the C11 memory
  model.

## Fix

**Bug 1**: Check the return value of `pthread_create`. On failure:

- If zero threads started (`i == 0`): tear down primitives and return `-rc`
  to the caller (propagates `EAGAIN` / `EPERM`).
- If at least one thread started: set `p->n_threads = i` and
  `p->n_workers_created = i` so the pool operates at reduced width;
  signal existing workers and break.

**Bug 2**: Add an `n_workers_created` field to `VmafThreadPool` that is
written once at creation and never decremented. `destroy` reads
`n_workers_created` (no lock needed — it is immutable after `create`)
instead of the mutable `n_threads`.

## Verification

- `meson test -C /tmp/build-tp` → 54/54 OK.
- `pre-commit run --files core/src/thread_pool.c` → all checks pass.

## Alternatives considered

**Capture `n_threads` under the lock in `destroy`**: would fix the race
without adding a new field. Rejected because it requires acquiring the lock
before broadcasting stop, which changes the existing lock-ordering and is
harder to reason about correctness across the cond-wait in `wait()`.

**Fail hard if any thread fails**: simpler than the partial-success path.
Rejected because one thread is enough for forward progress, and an EAGAIN
on thread N-1 should not discard the N-1 threads that started cleanly.
