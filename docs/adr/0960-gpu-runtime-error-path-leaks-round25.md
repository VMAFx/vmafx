<!-- markdownlint-disable MD060 -->
# ADR-0960: GPU runtime error-path leak fixes — round 25 (A.1 + A.2 + A.3)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: lusoris
- **Tags**: `cuda`, `memory`, `threading`, `correctness`, `fork-local`

## Context

Round-25 audit of GPU and CPU runtime error paths identified three related
resource-management bugs in `core/src/cuda/common.c` and
`core/src/picture_pool.c`. All three are reachable only on error paths that
are infrequent in normal operation (driver failure, OOM), which is why they
survived existing test coverage.

**Bug A.1 — CUDA stream leaked on `cuCtxPopCurrent` failure**

In both `init_with_primary_context` and `init_with_provided_context` in
`core/src/cuda/common.c`, the stream (`cu_state->str`) is created with
`cuStreamCreateWithPriority` and then the context is popped with
`cuCtxPopCurrent`. If `cuCtxPopCurrent` fails, the goto target was
`fail_after_pop`, which releases the primary context (in the primary-context
variant) but never calls `cuStreamDestroy`. The stream handle is
subsequently unreachable, causing a driver-side resource leak.

**Bug A.2 — `picture_pool.c` return-to-pool path missing `pthread_cond_signal`**

`vmaf_picture_pool_fetch` pops a free-list index then may fail later
(OOM on the `priv` malloc, or any error from `vmaf_picture_set_release_callback`
or `vmaf_ref_init`). On these error paths the function jumps to
`return_to_pool`, pushes the index back to `pool->free_list`, and unlocks.
The pre-fix code never called `pthread_cond_signal(&pool->available)`.
Any thread already sleeping in `pthread_cond_wait` (pool exhausted) would
never wake, causing an indefinite deadlock. This is the same class of bug
described in PR #1415 / ADR-0607 (`feedback_shared_resource_outlive_worker_scope`):
a shared resource (free-list slot) was returned but the owner (waiting thread)
was not notified.

**Bug A.3 — dangling `pic->priv` after failed fetch**

In the same `vmaf_picture_pool_fetch` function, `pic->priv` is set on line 232
before the two calls that can fail. On failure, `free(priv)` was called but
`pic->priv` was not nulled. Any code path that inspects `pic->priv` after a
failed fetch would dereference freed memory (undefined behaviour, detectable
by ASan or valgrind).

## Decision

Apply the following targeted fixes:

1. **A.1**: Add a new label `fail_after_stream` between `fail` and
   `fail_after_pop` in both `init_with_primary_context` and
   `init_with_provided_context`. The new label calls
   `cuStreamDestroy(cu_state->str)` (guarded by a NULL check) and zeros
   `cu_state->str`. The `cuCtxPopCurrent` on the success path now jumps to
   `fail_after_stream` instead of `fail_after_pop`.

2. **A.2**: Add `pthread_cond_signal(&pool->available)` in the
   `return_to_pool` block in `vmaf_picture_pool_fetch`, between the
   `pool->free_list` push and the `pthread_mutex_unlock`.

3. **A.3**: Add `pic->priv = NULL;` after `free(priv)` in both error branches
   of `vmaf_picture_pool_fetch` (the `vmaf_picture_set_release_callback`
   failure branch and the `vmaf_ref_init` failure branch).

Add a new CPU-gated test `core/test/test_picture_pool_error_paths.c` that
covers:

- `test_pool_fetch_priv_not_null_on_success` — confirms the A.3 fix does not
  clobber `pic->priv` on the success path.
- `test_pool_fetch_unref_refetch` — confirms the A.2 signal path allows
  re-fetch after unref (deadlock regression guard).
- `test_pool_waiter_woken_on_unref` — spawns a waiter thread on an exhausted
  pool and verifies it wakes when the holder releases via `vmaf_picture_unref`.

Direct injection of the error-path `return_to_pool` signal (A.2, OOM path)
and the dangling-priv null-out (A.3) would require linker-level malloc
interception; this is documented in the test file's preamble as infeasible in
the current harness.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| Refactor both init functions to use RAII-style cleanup list | Eliminates goto labels; cleaner unwind | Non-trivial refactor; deviates from the existing goto-unwind convention used throughout the file | The existing pattern is correct when labels are ordered properly; targeted fix is lower-risk |
| Wrap `vmaf_picture_pool_fetch` in a retry loop rather than signal | Avoids the condvar issue by never blocking | Busy-waits; wastes CPU; changes observable behaviour | Correctness fix is always preferable to a workaround |
| Null `pic->priv` unconditionally at function entry | Simpler | Would clobber a valid pointer if the caller reuses the struct without zeroing | Point-of-error null is safer and self-documenting |

## Consequences

- **Positive**: Three resource-management bugs removed. The CUDA stream
  leak (A.1) is particularly important under driver-error injection or OOM
  scenarios where init is retried. The pool deadlock (A.2) could manifest in
  long-running vmaf-tune jobs with small pools and concurrent threads.
- **Negative**: Minimal — the new `fail_after_stream` label adds four lines
  per function; the signal and null add one line each.
- **Neutral / follow-ups**: The new test adds a 50 ms `nanosleep` in
  `test_pool_waiter_woken_on_unref`; this is necessary to let the waiter
  thread reach `pthread_cond_wait` before the holder is released.

## References

- `feedback_shared_resource_outlive_worker_scope` (PR #1415, ADR-0607):
  canonical pattern — a worker's cleanup must not silently discard a shared
  resource (free-list slot, semaphore count) without notifying dependents.
- Round-25 audit findings A.1, A.2, A.3.
- `core/src/cuda/common.c` (Netflix#1300 original fix context).
- `core/src/picture_pool.c` (successful release path at line 76 already
  called `pthread_cond_signal`; the error path did not — asymmetry is the
  root cause of A.2).
