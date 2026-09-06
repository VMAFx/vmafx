# Research digest: where should the CUDA drain batch live?

- **Date**: 2026-09-06
- **Task**: `T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03`
- **Decisions produced**: [ADR-1187](../adr/1187-cuda-drain-batch-state-owned.md),
  [ADR-1189](../adr/1189-score-at-index-eagain-contract.md)

## Question

The T-GPU-OPT-1 fence batch (PR #312, `core/src/cuda/drain_batch.{c,h}`) stored
its registration table in `static _Thread_local DrainBatchTls g_drain_batch`.
Is thread scope the right scope, and if not, what replaces it?

## What the batch actually holds

Two borrowed things per entry:

| Field | What it is | Who owns it |
| --- | --- | --- |
| `finished[i]` | `CUevent` recorded on an extractor's private stream | the extractor's state, created in its `init()`, destroyed in its `close()` |
| `flags[i]` | `bool *` into the extractor's state (`&lc->drained`, or a legacy `&s->drained`) | same |
| `drain_str` | `CUstream` created lazily from `cu_state->ctx` | the `VmafCudaState` whose context created it |

Every one of those is reachable from `fex->cu_state`, which
`core/src/libvmaf.c` sets to `&vmaf->cuda.state` — the context's own copy of
the imported backend state. Nothing in the batch has thread lifetime; all of it
has `VmafCudaState` lifetime.

## Why the mismatch was reachable

`read_pictures_extractor_loop_cuda` ends with the batch open on purpose:

```text
Phase 1  flush(prev frame's events) -> collect() each -> close()
Phase 2  open() -> submit() each (each submit registers its event)
         return with the batch OPEN, n == number of CUDA extractors
```

Only the *next* call reaches Phase 1 again. Any exit that is not another
`vmaf_read_pictures` leaves the batch populated. `vmaf_close()` then ran, in
order: `feature_extractor_vector_destroy()` (frees the events and the flag
storage), `vmaf_close_backends()` → `vmaf_cuda_drain_batch_thread_destroy()`
(destroys only `drain_str`; `open`, `n` and the table survive because they are
thread-local, not state-local). The next `VmafContext` on that thread hit
Phase 1 of its first frame and walked the dead table.

The CUDA driver recycles event handles, so `cuStreamWaitEvent` frequently
*succeeds* against an unrelated live event, and the flush then executes
`*flags[i] = true` on freed memory. When the handle is not recycled the call
fails with `CUDA_ERROR_INVALID_HANDLE` and `vmaf_read_pictures` returns an
error for a perfectly good frame. Both shapes match the intermittent
wrong-score / NaN symptom in Netflix/vmaf#1305.

## Scope candidates

| Scope | Outlives the handles? | Needs locking? | Multi-state isolation |
| --- | --- | --- | --- |
| OS thread (`_Thread_local`) | **yes** — the bug | no | none: two states on one thread share one batch |
| Process global | yes | yes | none |
| `CUcontext`-keyed map | maybe (primary context is shared) | yes | partial |
| `VmafContext` | no | no | yes, but `drain_batch.c` cannot see the context layout |
| `VmafCudaState` (**chosen**) | no | no — the type is documented single-threaded | yes |

`VmafCudaState` is the narrowest scope that every registration site already
holds a pointer to, and it is opaque in the public header, so embedding the
batch is not an ABI break.

## The by-value import caveat

`vmaf_cuda_import_state` does `vmaf->cuda.state = *cu_state;` — a struct copy,
not a pointer take. Three consequences, all verified against the tree:

1. The batch the engine drives is the **context's copy**, because
   `fex->cu_state == &vmaf->cuda.state` (`core/src/libvmaf.c`, the
   `fex_ctx->fex->cu_state = &(vmaf->cuda.state)` assignment).
2. The drain stream is therefore created in and destroyed from that copy, in
   `vmaf_close_backends()` — the same place and order as before, so the
   "destroy the stream while the context is still alive" ordering constraint
   is preserved.
3. The caller's original `VmafCudaState` keeps a zeroed batch for its whole
   life. Two contexts importing one state get two independent batches, which
   is strictly better than the old shared-per-thread behaviour.

`VifStateCuda` also embeds a `VmafCudaState` **by value**
(`core/src/feature/cuda/integer_vif_cuda.h`). That copy must never be used for
registration; the vif submit path was already using `fex->cu_state` for its
`CudaFunctions *`, and the registration call now passes the same pointer.

## The public-reader half

`vmaf_score_at_index` / `vmaf_feature_score_at_index` /
`vmaf_score_at_index_model_collection` read the collector with no in-flight
check. `vmaf_feature_collector_get_score` takes the collector mutex, so this is
**not** a data race — the exposure is a stale read that the model paths turn
into a fabricated prediction via `vmaf_predict_score_at_index`.

Two fence designs were prototyped on paper and rejected (see ADR-1189's
alternatives table): a real fence would have to run `collect()` from a getter,
which double-writes the collector on the next frame's Phase 1; and a
`vmaf_thread_pool_wait` would deadlock any caller invoking the reader from a
`VmafMetadataConfiguration` callback, because those callbacks execute on pool
worker threads (`vmaf_feature_collector_append` →
`feature_collector_dispatch_metadata`). The `-EAGAIN` contract has neither
failure mode.

## Priority-clamp nit found in the same file set

`core/src/cuda/common.c` created the compute stream with
`prio2 = MAX(low, MIN(high, prio))`, `prio = high`. `cuCtxGetStreamPriorityRange`
returns `(leastPriority, greatestPriority)` and CUDA's scale is inverted
(smaller number = higher priority), so `high <= low` and the expression
collapses to `low` — the *lowest* priority, the opposite of what the comment
above it says. Measured on the RTX 4090 in this workstation: range is
`least = 0, greatest = -5`, and `cuStreamGetPriority(cu_state->str)` returned
`0` before the fix and `-5` after.

## Verification

`core/test/test_cuda_drain_batch_state_scope.c`, GPU-gated, three cases:

1. a flush issued through state B must not drain a registration made through
   state A;
2. teardown must empty an abandoned open batch, not merely drop its stream;
3. the compute stream must be created at CUDA's greatest priority.

All three fail against `origin/master`'s `drain_batch.c` (API-adapted probe)
and pass after the change. Raw output is quoted in the PR description.
