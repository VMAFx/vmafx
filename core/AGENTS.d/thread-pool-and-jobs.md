---
paths:
  - core/src/thread_pool.c
  - core/src/thread_pool.h
invariant: Thread pool enforces bounded admission, job recycling, and inline buffer reuse.
area: engine-and-runtime
---
<!-- markdownlint-disable MD013 MD060 -->
# Thread pool admission, job recycling, and buffer recycling

- **Bounded thread-pool admission** (Netflix `8fc71e3`, fork lifetime adaptation):
  `src/thread_pool.c` admits at most one queued job per successfully created
  worker, before payload allocation. Keep dequeue wakeups and checked condition
  teardown. Destruction waits for both workers and already-blocked producers;
  waking producer does not make it safe to free its mutex. Preserve
  cancellation/lifetime and mixed-payload tests in
  `test/test_thread_pool_backpressure.c`. Callbacks must not enqueue into
  same pool. Serialize destruction against API entry, including mutex-acquisition
  waiters; only proven registered capacity waiters can be cancelled concurrently. See
  [thread-pool behavior](../../docs/development/thread-pool.md).
- **Thread-pool job recycling + inline data buffer** (fork-local,
  ADR-0147): [`src/thread_pool.c`](../src/thread_pool.c) recycles
  `VmafThreadPoolJob` slots via `pool->free_jobs` free list
  (mutex-protected by `queue.lock`), stores payloads ≤
  `JOB_INLINE_DATA_SIZE` (64 bytes) inside `job->inline_data`
  instead of second `malloc`. Cleanup path distinguishes
  inline from heap payloads via
  `job->data != job->inline_data` guard in
  `vmaf_thread_pool_job_clear_data`; never collapse this
  check during rebase — freeing `inline_data` would corrupt
  slot. Fork's `func(void *data, void **thread_data)`
  signature and `VmafThreadPoolWorker` per-worker-data path must
  survive any upstream sync; Netflix upstream PR #1464 (closed)
  has similar job-pool but uses bare
  `func(void *data)` signature — on conflict keep fork's
  two-arg signature, merge only pool-mechanics changes.
  Struct carries immutable `n_workers_created` field (written
  once in `pool_create`, never decremented) alongside live
  `n_threads` counter (decremented by each exiting runner thread under
  `queue.lock`). `destroy` reads `n_workers_created` — not `n_threads`
  — to iterate `workers[]` for `thread_data_free`; never collapse
  these two counters back into one during rebase or `destroy`
  path reacquires data race (C11 UB, TSan-detected). See
  Worker-private extractor teardown has fallible prepare callback:
  `thread_data_prepare` closes every private context while workers and their
  owner array remain alive. `vmaf_thread_pool_destroy` returns first
  prepare error without stopping or freeing pool; `thread_data_free` is
  commit-only and runs after successful retry. Never move close calls back
  into void free callback.
  [Research-0097](../../docs/research/0097-thread-pool-pthread-create-unchecked-2026-05-10.md).
  See [ADR-0147](../../docs/adr/0147-thread-pool-job-pool.md) and
  [rebase-notes 0040](../../docs/rebase-notes.md).
