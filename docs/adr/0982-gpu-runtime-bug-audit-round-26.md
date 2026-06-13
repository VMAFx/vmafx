<!-- markdownlint-disable MD013 MD041 MD060 -->

# ADR-0982: GPU runtime bug audit — round 26 (init/teardown leak sweep)

- **Status**: Accepted
- **Date**: 2026-05-31
- **Deciders**: Lusoris
- **Tags**: `cuda`, `sycl`, `gpu`, `lifecycle`, `audit`

## Context

PR #470 / #482 closed the highest-confidence CUDA leaks on init failure paths (round-23 audit) and PR #489 the corresponding SYCL ones. The fork now has four GPU runtimes in tree (`core/src/cuda`, `core/src/sycl`, `core/src/hip`, `core/src/metal`) plus two shared TUs (`gpu_picture_pool.c`, `gpu_dispatch_env.c`). A focused re-read of every error-path goto / partial-init unwind in these surfaces surfaced six additional defects that share the same shape: an early success-path step (stream / event / device pointer / function table / extractor registration / VA image lifetime) is allocated, a later step fails, and the unwind drops past the cleanup for the early step. Most leak GPU resources on every retry — caller-visible "transient driver fault → workload survives but device-memory pressure grows" symptom.

## Decision

We will bundle the six findings into one PR and fix each at the unwind point that already exists (no API changes, no behaviour deltas on the success path). Specifically:

1. **`cuda/drain_batch.c::drain_stream_ensure`** — destroy the drain stream we just created when `cuCtxPopCurrent` fails on the success path. Previously the `fail_after_pop` label only NULL'd `g_drain_batch.drain_str`.
2. **`cuda/picture_cuda.c::vmaf_cuda_picture_alloc`** — zero `priv` before any CUDA call so partial-init unwind can use NULL sentinels, then destroy the upload stream + ready / finished events + every successfully-allocated device pointer at the `fail` label. Previously the unwind only freed `priv`.
3. **`cuda/common.c::vmaf_cuda_release`** — release the dlopen'd `CudaFunctions` table on the error path. Previously a `cuCtxPopCurrent` or `cuDevicePrimaryCtxRelease` failure dropped to `fail_after_pop` and returned the error code, leaving the function table allocated.
4. **`gpu_picture_pool.c::vmaf_gpu_picture_pool_init`** — unwind per-slot allocations on the first failure and free the pool struct. Previously the loop OR-aggregated all callback results then returned mid-initialised state with `*pool` set; callers (e.g. `picture_sycl.cpp`) propagated the error code, called `delete wrap`, and leaked per-slot device memory, the mutex, and `p->pic`.
5. **`sycl/common.cpp::vmaf_sycl_graph_register`** — create the lazy compute queue **before** pushing the extractor entry. Previously the order was reversed, so a queue-create failure left a registered extractor with no queue; every subsequent `graph_submit` asserted then dereferenced a null queue.
6. **`sycl/dmabuf_import.cpp::vmaf_sycl_import_va_surface_readback`** — wrap the SYCL submits + wait so a thrown `sycl::exception` cannot escape with the VA image still mapped and allocated.

Plus one cleanup: remove a stray `// test` trailing comment from `sycl/common.cpp` (introduced during graph-replay rebases, ADR-0840 follow-up).

## Alternatives considered

| Option | Pros | Cons | Why not chosen |
|---|---|---|---|
| One PR per backend (4 PRs) | Smaller diffs | Triples reviewer load, defeats round-audit framing | Round-audit framing trumps PR size on lifecycle sweeps |
| Defer to next bug-fix release | Less churn | Each defect is a slow leak under repeated driver faults; no upside | Leaks compound across CI sessions |
| Refactor every backend onto a single C++ RAII wrapper | Long-term cleanest | Touches every kernel TU; high blast radius; cross-backend ABI rewrite | Scope-explosion; not what the audit asked for |

## Consequences

- **Positive**: every fixed path is now allocation-neutral on error — a transient driver fault no longer leaks the corresponding GPU resource on retry. `gpu_picture_pool_init` failure no longer requires every caller to know to call `_close` on a half-init pool.
- **Negative**: `vmaf_cuda_release` now releases the `CudaFunctions` table on error too — a caller that inspects `cu_state->f` after a failed release will see NULL. The function table memset already zeroed the struct on the success path, so this just extends the behaviour to the error path; no caller in tree relies on the pre-release contents.
- **Neutral / follow-ups**: round-27 audit candidates surfaced during the read (CUDA `vmaf_cuda_kernel_lifecycle_init` documented partial-handle leak; SYCL `dispatch_strategy.cpp` raw getenv vs the snapshotting helper) are left in place — the inline comments / ADRs already document them as intentional.

## References

- req: "Deep bug audit on GPU runtime surfaces. Bundle into ONE DRAFT PR." (session req, 2026-05-31)
- [ADR-0960](0960-cuda-init-leak-audit-round-25.md) — round-25 CUDA init-leak audit (precedent)
- [ADR-0840](0840-gpu-dispatch-env-snapshot.md) — env snapshot helper context for #5
- [Netflix#1300](https://github.com/Netflix/vmaf/issues/1300) — original CUDA init leak series
