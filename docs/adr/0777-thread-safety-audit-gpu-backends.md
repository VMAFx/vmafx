<!-- Copyright 2026 Lusoris — BSD-3-Clause-Plus-Patent -->
# ADR-0777: Thread-Safety Audit — CUDA / SYCL / HIP Backends

| Field | Value |
| ----------- | ----------------------------------------------- |
| Status | Accepted |
| Date | 2026-05-29 |
| Tags | cuda, sycl, hip, thread-safety, audit, research, fork-local |

---

## Context

The fork ships three GPU backends (CUDA, SYCL, HIP) plus an internal
`VmafThreadPool` for CPU-side parallel extraction.  As cloud-native
deployment increases (ADR-0701, ADR-0703, ADR-0709), callers may invoke
the C ABI from multiple threads.  No prior audit assessed whether doing so
is safe, and no thread-safety contract appears in the public headers.

A code audit was requested covering:

1. Concurrent `vmaf_init` / `vmaf_close` / `vmaf_read_pictures` on the same context.
2. Per-extractor state sharing across internal worker threads.
3. CUDA context / SYCL queue ownership model.
4. Process-wide static globals and their protection.
5. The documented (or absent) thread-safety contract in the public ABI.

Full findings: [`docs/research/thread-safety-audit-backends-2026-05-29.md`](../research/thread-safety-audit-backends-2026-05-29.md).

---

## Decision

Record the audit findings as an ADR without any code changes.
No fix is applied in this PR.  Follow-up work items are enumerated in
the Consequences section below.

---

## Findings (summary)

### Q1 — Concurrent vmaf_init / vmaf_close / vmaf_read_pictures

`VmafContext` is a flat heap-allocated struct with no internal lock.  Each
allocated handle is safe to use from one thread at a time.  Multiple
independent handles (one per thread) are safe.  Sharing a single handle
across threads without external serialization is a data race.  `vmaf_init`
itself is safe to call concurrently (allocates independent structs).
`vmaf_close` drains the thread pool before teardown.

### Q2 — Per-extractor state sharing

The pool creates one `VmafFeatureExtractorContext` (with its own `priv`
allocation) per worker slot — that private state is not shared.  The
shared `VmafFeatureExtractor` descriptor carries `fex->prev_ref` and
`fex->gpu_pending`, which the `VMAF_BATCH_THREADING` path writes from
concurrent worker threads — a latent data race.

### Q3 — CUDA context / SYCL queue ownership

**CUDA:** Single `VmafCudaState` per `VmafContext`, one primary CUcontext
(process-wide) and one `CUstream`.  `cuCtxPushCurrent` / `cuCtxPopCurrent`
serializes GPU work within a single context.  Per-thread drain stream is
`_Thread_local` — safe.

**SYCL:** `VmafSyclState` owns a primary queue and a copy queue.  Mutable
frame-counter fields (`cur_upload`, `cur_compute`, `submit_count`,
`frame_counter`, etc.) in `VmafSyclState` are written without a lock.
Concurrent calls to `vmaf_sycl_shared_frame_upload` or
`vmaf_sycl_graph_submit` from multiple OS threads on the same state object
are data races.

**HIP:** Scaffold-only; no runtime queue object exists.  Same lifetime
contract as SYCL/Vulkan (caller owns state, clears pointer on close).

### Q4 — Static globals

| File | Variable | Race-safe? |
| --- | --- | --- |
| `core/src/log.c` | `vmaf_log_level`, `istty` | No — plain `int`/`enum`, no `_Atomic`, no lock |
| `cuda/dispatch_strategy.c` | `g_env_disp` | Yes — `pthread_once` |
| `gpu_dispatch_env.c` | `g_rows[]` | Yes — `pthread_mutex_t` |

`vmaf_log_level` is written by `vmaf_init` → `vmaf_set_log_level` and read
by every `vmaf_log` call from worker threads.  Not `_Atomic`; technically
a C11 data race.  Low practical impact (write converges immediately at
init).

### Q5 — Public ABI documentation

No thread-safety contract exists in `core/include/libvmaf/libvmaf.h`.
The upstream Netflix API has the same gap.

---

## Alternatives considered

**Apply fixes in this PR** — deferred.  The audit scope was read-only; fixes
require design review for the SYCL frame-counter race and the log-level
`_Atomic` conversion.

---

## Consequences

The following follow-up work items are created (not yet ticketed in BACKLOG):

1. **Thread-safety contract in the public header** — add a `@thread-safety`
   paragraph to the `vmaf_init` / `vmaf_close` / `vmaf_read_pictures` doc
   comments in `core/include/libvmaf/libvmaf.h`.
2. **`vmaf_log_level` / `istty` — make `_Atomic`** — trivial one-liner in
   `log.c`, eliminates the C11 data race.
3. **SYCL frame-counters — add mutex or restrict to single-producer model** —
   document or enforce that `vmaf_sycl_shared_frame_upload` and
   `vmaf_sycl_graph_submit` must be called from a single producer thread.
4. **`fex->prev_ref` in `VMAF_BATCH_THREADING`** — hoist `prev_ref` into the
   per-thread `BatchThreadData` to eliminate the write-on-shared-descriptor
   pattern.

---

## References

- Research digest: `docs/research/thread-safety-audit-backends-2026-05-29.md`
- `core/src/libvmaf.c` — `VmafContext`, `vmaf_init`, `vmaf_close`, `threaded_extract_func`
- `core/src/cuda/common.c` — `VmafCudaState` lifecycle
- `core/src/sycl/common.cpp` — `VmafSyclState`,
  `vmaf_sycl_shared_frame_upload`, `vmaf_sycl_graph_submit`
- `core/src/hip/dispatch_strategy.c` — HIP stub
- `core/src/log.c` — unprotected `vmaf_log_level`
- `core/src/gpu_dispatch_env.c` — mutex-protected dispatch env table
- `core/src/cuda/dispatch_strategy.c` — `pthread_once`-protected env cache
- `core/src/feature/feature_extractor.h` — `VmafFeatureExtractor.cu_state`,
  `.sycl_state`, `.priv`
