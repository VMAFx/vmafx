<!-- Copyright 2026 Lusoris — BSD-3-Clause-Plus-Patent -->
# Thread-Safety Audit: CUDA / SYCL / HIP Backends

**Date:** 2026-05-29
**Scope:** `core/src/cuda/`, `core/src/sycl/`, `core/src/hip/`, `core/src/libvmaf.c`,
`core/src/log.c`, `core/src/gpu_dispatch_env.c`
**ADR:** [ADR-0777](../adr/0777-thread-safety-audit-gpu-backends.md)

---

## 1. Can two threads safely call vmaf_init / vmaf_close / vmaf_read_pictures?

**Answer: No — concurrent use on the same `VmafContext *` is unsafe.**

`VmafContext` is a single flat struct allocated by `vmaf_init`.  All lifecycle
functions (`vmaf_init`, `vmaf_use_feature`, `vmaf_close`, `vmaf_read_pictures`)
read and write the same struct members without any lock.  `vmaf_init` is safe
to call concurrently from different threads as long as each call allocates its
own `VmafContext *`; there is no global init counter or process-wide lock
guarding the allocation.  The contract is identical to `fopen`: one handle per
thread; sharing a handle without external synchronization is a data race.

`vmaf_close` calls `vmaf_thread_pool_wait` first, so pending pool jobs are
drained before teardown — that is safe. But calling `vmaf_close` while another
thread is inside `vmaf_read_pictures` on the same context is a data race.

**Internal threads** (the `VmafThreadPool` workers created when
`cfg.n_threads > 0`) are managed internally and are safe: each worker operates
on an independently acquired `VmafFeatureExtractorContext` from the pool.

---

## 2. Are per-extractor state structures shared across threads?

**Answer: `VmafFeatureExtractor` (the static descriptor) is shared;
`VmafFeatureExtractorContext` and `fex->priv` are per-pool-slot.**

The `VmafFeatureExtractor` struct (the static registry entry) carries
`fex->cu_state` and `fex->sycl_state` as mutable pointer fields that are set
once during `vmaf_use_feature` / `vmaf_use_features_from_model` (lines 424,
600, 1531, 1534 of `libvmaf.c`). Those writes happen before the frame loop, so
there is no concurrent writer once processing begins.  However, `fex->prev_ref`
and `fex->gpu_pending` are frame-local state also stored on the shared
descriptor — the `VMAF_BATCH_THREADING` path writes `fex->prev_ref` on the
shared `fex` pointer for each pool thread (lines 1752, 1759), which is a
latent data race when multiple batch threads run the same `fex` concurrently.
The non-batch path acquires a per-thread `VmafFeatureExtractorContext` from the
pool (one per worker, keyed by `fex` identity), and `priv` is heap-allocated
once per pool slot (`feature_extractor.c` line 532), so private state is not
shared between concurrent workers in the standard path.

---

## 3. Are CUDA contexts / SYCL queues per-thread or shared?

**CUDA:** The single `VmafCudaState` embedded in `VmafContext` holds one
`CUcontext` and one `CUstream` (line 77, `common.c`).  The context is
created via `cuDevicePrimaryCtxRetain` (shared primary context model) or
imported from the caller.  All CUDA operations use `cuCtxPushCurrent` /
`cuCtxPopCurrent` for thread attachment — the push/pop pattern is
inherently per-call but operates on the single shared primary context.
The drain-batch path uses `_Thread_local DrainBatchTls g_drain_batch`
(`drain_batch.c` line 49) — a thread-local drain stream per OS thread,
which is safe; but that stream is created/destroyed inside
`vmaf_cuda_drain_batch_thread_destroy`, not under a lock, so two threads
calling `vmaf_close` simultaneously would race on teardown.

> **Update 2026-09-06 — this paragraph is historical.** The thread-local
> batch was the second half of `T-UPSTREAM-1305`: thread scope is *wider*
> than the CUevents and `bool *`s the batch stores, so an abandoned context
> handed freed handles to the next context on the same thread. The batch now
> lives in `VmafCudaState::drain_batch` and is destroyed with the state —
> see [ADR-1187](../adr/1187-cuda-drain-batch-state-owned.md) and
> [`2030-cuda-drain-batch-ownership-2026-09-06.md`](2030-cuda-drain-batch-ownership-2026-09-06.md).
> The teardown race described above is dissolved rather than fixed: two
> threads closing two contexts now touch two different batches.

**SYCL:** `VmafSyclState` owns two `sycl::queue` objects (primary +
copy queue, lines 67-68 of `common.cpp`) and one optional `combined_queue`
created lazily in `vmaf_sycl_graph_register`.  SYCL queues are
reference-counted internally, but `VmafSyclState` mutates `cur_upload`,
`cur_compute`, `frame_counter`, `submit_count`, and `submit_frame` without
any lock (only `profiling_lock` is a `std::mutex`).  Concurrent calls to
`vmaf_sycl_shared_frame_upload` or `vmaf_sycl_graph_submit` from multiple
threads on the same `VmafSyclState *` are data races.

**HIP:** The HIP backend is in early scaffold state.  `VmafHipState *` is
stored on `VmafContext` and cleared (not freed) by `vmaf_close`.  No HIP
runtime queue/stream object has been shipped yet; `vmaf_hip_dispatch_supports`
is a stub that always returns 0.  No concurrency analysis is possible beyond
the same lifetime-contract as SYCL/Vulkan.

---

## 4. Are there static globals that would race?

Four process-wide statics were identified:

| Location | Variable | Protection |
| --- | --- | --- |
| `core/src/log.c:34` | `vmaf_log_level` (enum) | **None** — plain assignment in `vmaf_set_log_level`; concurrent reads from worker threads during `vmaf_log` are technically a data race under C11 (not `_Atomic`). |
| `core/src/log.c:35` | `istty` (int) | **None** — same issue as `vmaf_log_level`. |
| `core/src/cuda/dispatch_strategy.c:38-40` | `g_env_once` + `g_env_disp` | Protected by `pthread_once` / `INIT_ONCE`. Safe. |
| `core/src/gpu_dispatch_env.c:35,60` | `g_rows[]` + `g_lock` | Protected by `pthread_mutex_t` (fast path lockless on cached rows). Safe. |

The `log.c` statics (`vmaf_log_level`, `istty`) are written by
`vmaf_set_log_level` (called from `vmaf_init`) and read by every
`vmaf_log` invocation including from worker threads.  The current
implementation is not `_Atomic` and has no fence, so concurrent
`vmaf_init` + `vmaf_log` from different threads is a formal C11 data race.
In practice the value converges immediately and `vmaf_set_log_level` is
only called at init, so this has never triggered a bug, but it is not
standards-conformant.

---

## 5. Is the C ABI documented as thread-safe or not?

**No explicit thread-safety contract exists in the public headers.**

`core/include/libvmaf/libvmaf.h` documents `n_threads` as controlling how
many *internal* feature-extractor threads the library spawns, but contains
no statement about whether the `VmafContext *` handle itself is safe to
use from multiple caller threads simultaneously.  The upstream Netflix API
makes the same omission.

---

## Summary of risks

| Risk | Severity | Affected backends |
| --- | --- | --- |
| Concurrent `vmaf_read_pictures` / `vmaf_close` on same context | High (data race) | All |
| `fex->prev_ref` written from multiple batch threads on shared descriptor | Medium (latent) | VMAF_BATCH_THREADING CPU path |
| `vmaf_log_level` / `istty` unprotected concurrent read/write | Low (C11 UB, not crash-inducing in practice) | All |
| `VmafSyclState` mutable frame-counters unprotected | High (data race if caller parallelizes) | SYCL |
| CUDA drain-batch teardown without lock | Low (race on `vmaf_close` only if called from two threads) | CUDA |
| No public thread-safety contract documented | Informational | All |

No fix is applied in this PR; this document is the audit deliverable.
