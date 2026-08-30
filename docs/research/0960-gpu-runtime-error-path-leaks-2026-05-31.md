# Research digest: GPU runtime error-path leak fixes (round-25 audit)

**Date**: 2026-05-31
**ADR**: [ADR-0960](../adr/0960-gpu-runtime-error-path-leaks-round25.md)
**PR**: fix/gpu-runtime-error-paths-round25

## Findings

Round-25 audit reviewed error-path unwind logic in the CUDA backend runtime
and the CPU picture pool. Three resource-management defects were identified.

### A.1 — CUDA stream leak on `cuCtxPopCurrent` failure

**Location**: `core/src/cuda/common.c`, functions `init_with_primary_context`
(line 79) and `init_with_provided_context` (line 127).

**Root cause**: After `cuStreamCreateWithPriority` succeeds, the success path
calls `cuCtxPopCurrent(NULL)`. Prior to this fix the failure target was
`fail_after_pop`, which skips `cuStreamDestroy`. The stream handle stored in
`cu_state->str` is unreachable after `vmaf_cuda_state_init` fails, so the
driver-side CUstream object is never freed for the lifetime of the process.

**Severity**: Low in normal usage (CUDA init rarely fails in production), but
meaningful under driver-error injection, CI environments without a real GPU,
and any future retry logic.

**Fix**: Insert a `fail_after_stream` label that calls
`cuStreamDestroy(cu_state->str)` (NULL-guarded) before falling through to
`fail_after_pop`. Change the goto target on the `cuCtxPopCurrent` call from
`fail_after_pop` to `fail_after_stream`.

### A.2 — Picture pool `return_to_pool` missing `pthread_cond_signal`

**Location**: `core/src/picture_pool.c`, `vmaf_picture_pool_fetch`,
`return_to_pool` label (~line 250).

**Root cause**: On OOM or callback-init failure after the free-list pop,
`return_to_pool` pushes the index back and releases the lock but does not
call `pthread_cond_signal(&pool->available)`. Any thread already in
`pthread_cond_wait` (pool exhausted) will never wake. The successful release
path (`pooled_picture_release` callback, line 76) correctly calls
`pthread_cond_signal`, so the asymmetry is the root cause.

**Severity**: Medium. Would manifest as an indefinite deadlock in any
concurrent workload where the pool is exhausted and a fetch failure occurs
on one thread while another is waiting. vmaf-tune with `--max-concurrent-decodes`
and a small picture pool is the realistic trigger.

**Fix**: Add `pthread_cond_signal(&pool->available)` between the
`pool->free_list` push and the `pthread_mutex_unlock` in `return_to_pool`.

This is the same invariant pattern identified in PR #1415 (ADR-0607,
`feedback_shared_resource_outlive_worker_scope`): when a shared resource is
returned to a pool, all threads that may be waiting for that resource must be
notified.

### A.3 — Dangling `pic->priv` after failed fetch

**Location**: `core/src/picture_pool.c`, `vmaf_picture_pool_fetch`, two error
branches after `pic->priv` is set on line 232.

**Root cause**: `pic->priv = (VmafPicturePrivate *)priv` is assigned before
the calls to `vmaf_picture_set_release_callback` and `vmaf_ref_init`. Both
error branches call `free(priv)` and jump to `return_to_pool`, but neither
zeroed `pic->priv`. The caller receives a non-zero error code but also a
`*pic` struct with a freed pointer in `priv`.

**Severity**: Low — only exploitable if a caller inspects `pic->priv` after a
failed fetch, which is non-conformant use of the API. However the contract
"output parameters are undefined after error return" should be satisfied in
practice via nulling, and ASan will flag any access.

**Fix**: Add `pic->priv = NULL;` after `free(priv)` in both branches.

## Test coverage

A new test `core/test/test_picture_pool_error_paths.c` covers:

- `test_pool_fetch_priv_not_null_on_success`: verifies A.3 fix does not
  accidentally null-out `priv` on the success path.
- `test_pool_fetch_unref_refetch`: exercises the full cond-signal path by
  doing a single-pool fetch → unref → re-fetch cycle; would deadlock if the
  signal were missing.
- `test_pool_waiter_woken_on_unref`: spawns a waiter thread on an exhausted
  pool, releases the holder, and asserts the waiter wakes within a bounded
  time.

Direct injection of the error-path `return_to_pool` branch (OOM after the
free-list pop) is not feasible without linker-level malloc interception. The
coverage gap is documented in the test file's preamble.

For A.1, CUDA init failure under `cuCtxPopCurrent` is not testable in the
CPU-only fast suite. The fix is verified to compile cleanly with
`-Denable_cuda=true`.

## References

- ADR-0607 / PR #1415: canonical pool-return + notify pattern.
- `core/src/cuda/common.c` Netflix#1300 context (original stream-init fixes).
