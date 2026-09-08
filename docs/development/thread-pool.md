# Thread-pool behavior

The CPU batch reader uses a bounded worker queue. It accepts at most one pending
job per successfully created worker, in addition to running jobs. A producer
waits when that queue is full and resumes when a worker takes a job. This keeps
a decoder that outpaces feature extraction from accumulating an unbounded
backlog of retained pictures. It does not bound all memory used by scoring.

No new option is needed. The worker count comes from the existing `n_threads`
configuration (`--threads` in the CLI). If the system can create only part of
the requested worker set, the queue uses that actual worker count.

## Internal caller contract

`vmaf_thread_pool_enqueue()` may block before copying its payload. Keep the
input buffer alive until it returns; the caller may then reuse it. Worker
callbacks must not recursively enqueue into the same pool or depend on work
that a blocked producer has yet to submit. The current production caller is
`threaded_read_pictures_batch()` in `core/src/libvmaf.c`; its worker processes
the submitted batch without recursively submitting work.

Call `vmaf_thread_pool_wait()` to drain accepted jobs and collect worker errors.
Errors remain OR-accumulated and are cleared after that wait, preserving the
existing batch contract.

`vmaf_thread_pool_destroy()` discards pending jobs and finishes running jobs.
It wakes producers already blocked on capacity, returns `-ECANCELED` to them,
and waits for them to leave before freeing synchronization objects. The caller
must prevent new API calls once destruction begins; arbitrary new calls racing
with a freed handle are outside the lifetime contract. `vmaf_close()` drains
its thread pool before destroying it.

## Regression checks

With an existing CPU Meson build:

```bash
meson test -C build test_thread_pool test_thread_pool_backpressure --print-errorlogs
```

The backpressure suite holds a worker until the producer demonstrably reaches
the capacity wait. It checks reduced worker-creation capacity, dequeue wakeup,
shutdown with a deliberately delayed producer, two concurrent producers with
mixed inline/heap payloads, batch-error reset, per-worker cleanup and primitive
initialization failure. Readiness uses condition signals and deadlines. The
suite runs under the normal fast and sanitizer test selection.

The [research note](../research/thread-pool-backpressure.md) records the upstream
fix and the fork-specific adaptation. No score formula or golden assertion is
changed.
