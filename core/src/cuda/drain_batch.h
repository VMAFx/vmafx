/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA fence-batching helpers (T-GPU-OPT-1, ADR-0242).
 *
 *  Background
 *  ----------
 *  Each kernel-template-based CUDA feature extractor records a private
 *  ``finished`` event on its drain stream after its DtoH readback
 *  (see ``kernel_template.h`` :: ``VmafCudaKernelLifecycle``). The
 *  legacy collect path called ``cuStreamSynchronize(lc->str)`` once
 *  per extractor — N driver round-trips per frame, where N is the
 *  number of co-scheduled CUDA extractors (3 for the
 *  ``vmaf_v0.6.1`` model, up to 12 with extra features enabled).
 *
 *  Optimization
 *  ------------
 *  This module batches those N round-trips into a single
 *  ``cuStreamSynchronize`` on a shared drain stream:
 *
 *      drain_open()                 — engine enters submit-all mode
 *      drain_register(lc) × N       — extractors join during submit()
 *      drain_flush(cu_state)        — engine waits all N events at once
 *                                     and marks each lifecycle drained
 *      drain_close()                — engine leaves the batch
 *
 *  After ``drain_flush``, ``vmaf_cuda_kernel_collect_wait`` skips its
 *  per-stream sync because the lifecycle's ``drained`` flag is set,
 *  so each ``collect()`` becomes a host-side buffer-read only.
 *
 *  Bit-exactness invariant
 *  -----------------------
 *  The optimization is **scheduling-only**. The same kernels execute
 *  on the same streams in the same order; only the host-side wait
 *  point changes. CUDA cross-stream ordering is preserved by
 *  ``cuStreamWaitEvent`` on each registered ``finished`` event before
 *  the single sync. Bit-exact tolerance: places=4 (fork-wide gate).
 *
 *  Concurrency
 *  -----------
 *  All state is **thread-local** — each orchestrator thread keeps
 *  its own batch. The library's frame loop is single-threaded for
 *  the GPU dispatch path (the ``thread_pool`` parallelisation is for
 *  CPU extractors only — see ``read_pictures_should_skip``), so the
 *  TLS scope matches the actual call graph.
 *
 *  Failure mode
 *  ------------
 *  If ``drain_register`` overflows the static cap, the lifecycle is
 *  silently skipped and its ``drained`` flag stays cleared, so
 *  ``vmaf_cuda_kernel_collect_wait`` falls back to the per-stream
 *  sync. This is degraded but correct.
 */

#ifndef LIBVMAF_CUDA_DRAIN_BATCH_H_
#define LIBVMAF_CUDA_DRAIN_BATCH_H_

#include <stdbool.h>

#include "common.h"

#ifdef __cplusplus
extern "C" {
#endif

struct VmafCudaKernelLifecycle;

/* Maximum number of extractor lifecycles per drain batch. The fork
 * never registers more than ~16 simultaneous CUDA extractors; the
 * cap is set to 32 to leave headroom for ``--feature``-stacked
 * runs. Static-cap rather than dynamic alloc keeps the hot path
 * allocation-free. Overflow is degraded (per-extractor sync), not
 * fatal. */
#define VMAF_CUDA_DRAIN_BATCH_MAX 32

/**
 * Open a drain batch on the calling thread.
 *
 * Idempotent: a second open without an intervening close is a no-op
 * and keeps the existing batch.
 */
void vmaf_cuda_drain_batch_open(const VmafCudaState *cu_state);

/**
 * Register an extractor lifecycle into the open batch.
 *
 * Called by template-based extractors at the end of submit() (after
 * ``cuEventRecord(lc->finished, lc->str)``). When no batch is open,
 * this is a no-op so individual ``submit()`` calls outside the
 * orchestrator (e.g. unit tests) keep working unchanged.
 *
 * Returns 0 on success, 0 also when the batch is closed (no-op),
 * and -ENOSPC when the batch is full (lifecycle skipped, the caller
 * falls back to per-stream sync via ``vmaf_cuda_kernel_collect_wait``).
 */
int vmaf_cuda_drain_batch_register(struct VmafCudaKernelLifecycle *lc);

/**
 * Register a raw (event, drained-flag) pair into the open batch.
 *
 * Companion to ``vmaf_cuda_drain_batch_register`` for legacy
 * extractors that pre-date ``VmafCudaKernelLifecycle`` (currently
 * ``integer_motion_cuda``, ``integer_adm_cuda``, ``integer_vif_cuda``,
 * ``ssimulacra2_cuda``). They each carry their own ``s->finished``
 * CUevent + ``s->str`` private stream; this entry-point accepts the
 * event directly + a pointer to a ``bool`` the extractor's
 * ``collect()`` checks to decide whether to skip its
 * cuStreamSynchronize.
 *
 * Returns 0 on success, 0 when the batch is closed (no-op), or
 * -ENOSPC on overflow (caller falls back to per-stream sync).
 */
int vmaf_cuda_drain_batch_register_event(CUevent finished, bool *drained_out);

/**
 * Drain all registered lifecycles in one host-side wait.
 *
 *   - cuStreamWaitEvent(drain_stream, lc->finished, 0)  for each lc
 *   - cuStreamSynchronize(drain_stream)                 (single)
 *   - lc->drained = true                                for each lc
 *
 * After the call, every registered ``collect()`` will see
 * ``lc->drained == true`` and skip its private-stream sync. The
 * batch is **not** cleared — call ``vmaf_cuda_drain_batch_close``
 * after the collect-all phase to reset.
 *
 * No-op when no lifecycles are registered or the batch is closed.
 *
 * Returns 0 on success or a negative errno from
 * ``vmaf_cuda_result_to_errno`` on the first CUDA failure.
 */
int vmaf_cuda_drain_batch_flush(VmafCudaState *cu_state);

/**
 * Close the drain batch on the calling thread.
 *
 * Resets the registration list. Does not destroy the drain stream
 * — that lives until ``vmaf_cuda_drain_batch_thread_destroy`` (or
 * thread exit; the stream is owned by the persistent CUDA context
 * across calls and is reused on the next batch).
 */
void vmaf_cuda_drain_batch_close(void);

/**
 * Tear down the calling thread's drain stream.
 *
 * Called from the engine on context shutdown. Safe on a thread that
 * never opened a batch.
 */
void vmaf_cuda_drain_batch_thread_destroy(VmafCudaState *cu_state);

/**
 * Number of entries currently registered in this thread's batch.
 *
 * Introspection for the unit test that pins the owner scoping
 * (docs/state.md T-UPSTREAM-1305-CUDA-DRAIN-BATCH-THREAD-GLOBAL-2026-09-03):
 * a batch owned by another ``VmafCudaState`` must never be waited on, and
 * ``vmaf_cuda_drain_batch_thread_destroy`` must leave zero entries behind so
 * the next context on this thread cannot see freed events or flags. Not part
 * of the public libvmaf API — ``drain_batch.h`` is a private header.
 */
unsigned vmaf_cuda_drain_batch_pending(void);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_CUDA_DRAIN_BATCH_H_ */
