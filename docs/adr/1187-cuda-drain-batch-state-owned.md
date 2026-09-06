# ADR-1187: The CUDA drain batch is owned by `VmafCudaState`, not by the OS thread

- **Status**: Accepted
- **Date**: 2026-09-06
- **Deciders**: Lusoris
- **Tags**: cuda, gpu, lifetime, memory-safety

## Context

T-GPU-OPT-1 (PR #312) added a fence batch that collapses the per-extractor
`cuStreamSynchronize` calls of one frame into a single host-side wait. The
batch was stored as `static _Thread_local DrainBatchTls g_drain_batch` in
`core/src/cuda/drain_batch.c`, i.e. keyed by OS thread.

Every entry in that batch is a borrowed handle: a `CUevent` owned by a feature
extractor and a `bool *` pointing into that extractor's private state. Their
owner is the `VmafCudaState` the extractor is bound to (`fex->cu_state`), which
is a member of `VmafContext`. Thread scope is therefore strictly wider than the
scope of the things stored, and the frame loop makes the mismatch reachable:
`read_pictures_extractor_loop_cuda` deliberately returns with the batch **open**
and `n > 0` so the next frame's Phase-1 flush can wait on it.

A caller that abandons the context — an error return from `vmaf_read_pictures`,
a decode failure, or simply `vmaf_close()` without the terminal
`vmaf_read_pictures(NULL, NULL)` — never reaches that flush. `vmaf_close()`
destroyed the extractor vector (freeing the events and the `drained` bools) and
then called `vmaf_cuda_drain_batch_thread_destroy`, which only destroyed the
drain *stream*; `open`, `n` and the entry table survived. The next
`VmafContext` created on that thread flushed them: `cuStreamWaitEvent` on a
recycled handle, then `*flags[i] = true` into freed memory, or an outright
`CUDA_ERROR_INVALID_HANDLE`. This is `T-UPSTREAM-1305`, and it matches the
intermittent wrong-score / NaN symptom in Netflix/vmaf#1305.

Power-of-10 §6 ("declare data objects at the smallest possible scope") is the
direct constraint: the batch's storage duration has to be at most that of the
handles it aliases.

## Decision

We move the batch into the backend state it belongs to. `VmafCudaDrainBatch` is
declared in `core/src/cuda/common.h` and embedded as
`VmafCudaState::drain_batch`, and every drain entry point takes the owning
`VmafCudaState *`. `vmaf_cuda_drain_batch_thread_destroy` becomes
`vmaf_cuda_drain_batch_destroy` and empties the batch as well as destroying the
stream. `vmaf_close()` additionally runs a best-effort
`flush` + `close` on a still-open batch *before*
`feature_extractor_vector_destroy()`, so in-flight GPU work has landed before
the buffers it reads are freed.

`VmafCudaState` is opaque in `core/include/libvmaf/libvmaf_cuda.h`, so growing
it is not an ABI break. `vmaf_cuda_import_state` copies the caller's state into
the context **by value**, so the batch the engine drives is the context's own
copy and its drain stream is destroyed with that copy in
`vmaf_close_backends()`; the caller's original keeps an untouched zeroed batch,
and two contexts importing one state never share batch storage.

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
| --- | --- | --- | --- |
| Keep the batch thread-local, only add the `vmaf_close()` flush + close | Smallest diff; fixes the reachable use-after-free today | Leaves thread scope wider than the handles' owner. Any future path that abandons a context without going through `vmaf_close()` (a `vmaf_init` failure unwind, a crash-recovery harness, a caller that leaks the context) re-opens the same hole, and two `VmafCudaState`s driven from one thread still share one batch | Treats the symptom. The ledger's closure criteria name the ownership move explicitly |
| Key the batch by `CUcontext` in a process-wide map | Survives a state that is copied by value | Needs a lock (the map is process-wide), adds an allocation to the hot path, and `CUcontext` is not unique per `VmafContext` when the primary context is shared | More machinery than the problem needs, and it re-introduces global mutable state |
| Thread-local batch plus a generation counter validated at flush | No API churn in the extractors | Detects the stale batch instead of preventing it; a recycled generation still aliases freed memory | Detection where prevention is available is the weaker guarantee |
| Move the batch into `VmafContext` rather than `VmafCudaState` | The context is the true frame-loop owner | `drain_batch.c` would have to include `libvmaf.c`'s private context layout, and the extractors only ever see `fex->cu_state` | `VmafCudaState` is already the handle every registration site holds |

## Consequences

- **Positive**: the batch cannot outlive the handles it stores. Two
  `VmafCudaState`s on one thread are fully isolated. Abandoning a context is
  safe. The GPU-work fence in `vmaf_close()` also removes the (previously
  untested) window where extractor buffers were freed with kernels still
  reading them.
- **Negative**: five internal entry points gain a `VmafCudaState *` parameter,
  so the four registration sites (`kernel_template.h`,
  `integer_adm_cuda.c`, `integer_vif_cuda.c`, `integer_ms_ssim_cuda.c`) had to
  be touched. `VmafCudaState` grows by ~520 bytes; it is heap-allocated once
  per state and copied once per import, so this is not on any hot path.
- **Neutral / follow-ups**: bit-exactness is untouched — this changes *where*
  the batch lives, not which events are waited on or in what order.
  `core/test/test_cuda_drain_batch_state_scope.c` pins the three observable
  invariants. `docs/research/thread-safety-audit-backends-2026-05-29.md` §3
  carries an update note; its `_Thread_local` description is now historical.

This ADR also folds in an unrelated one-line defect found in the same audit:
`core/src/cuda/common.c` clamped the compute stream's priority as
`MAX(low, MIN(high, prio))`. CUDA's priority scale is inverted —
`cuCtxGetStreamPriorityRange` returns `(leastPriority, greatestPriority)` with
`greatest <= least` numerically — so that expression collapsed to `low`, i.e.
the primary-context path silently requested the *lowest* priority, the exact
opposite of the comment above it. The clamp is now `MIN(low, MAX(high, prio))`.

## References

- `T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03` in
  [`docs/state.md`](../state.md).
- Netflix/vmaf#1305 (the upstream report this was triaged from).
- PR #312 — T-GPU-OPT-1, the fence-batching optimisation this re-scopes. It
  shipped without an ADR; the `ADR-0242` citations in the CUDA comment blocks
  are a stale mis-reference (ADR-0242 is the tiny-AI training corpus) and are
  left alone here rather than churned tree-wide.
- [ADR-0271](0271-cuda-drain-batch-ms-ssim.md) — T-GPU-OPT-2, the sibling ADR
  that documents the helper's contract from the extractor side.
- [ADR-0519](0519-hip-import-state-implementation.md) — the
  `vmaf_<backend>_import_state` ownership contract this preserves.
- [`docs/research/2030-cuda-drain-batch-ownership-2026-09-06.md`](../research/2030-cuda-drain-batch-ownership-2026-09-06.md).
