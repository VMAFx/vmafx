/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA fence-batching helpers (T-GPU-OPT-1, PR #312; re-scoped to the
 *  backend state by ADR-1187).
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
 *      drain_open(cu_state)                 — engine enters submit-all mode
 *      drain_register(cu_state, lc) × N     — extractors join during submit()
 *      drain_flush(cu_state)                — engine waits all N events at once
 *                                             and marks each lifecycle drained
 *      drain_close(cu_state)                — engine leaves the batch
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
 *  Ownership and lifetime (ADR-1187)
 *  ---------------------------------
 *  The batch table and the drain stream live in
 *  ``VmafCudaState::drain_batch`` — see ``common.h`` — so they are
 *  owned by the backend state the registered handles belong to.
 *  Every entry aliases a ``CUevent`` and a ``bool *`` owned by an
 *  extractor bound to that state, so state-scoped storage is the
 *  only scope where the batch cannot outlive its contents.
 *
 *  This replaces the original ``static _Thread_local`` batch, which
 *  was keyed by OS thread: an abandoned or errored ``VmafContext``
 *  left the batch open with destroyed events and freed flag pointers
 *  in it, and the next ``VmafContext`` created on that thread flushed
 *  them (T-UPSTREAM-1305, Netflix/vmaf#1305).
 *
 *  ``vmaf_cuda_import_state`` copies the caller's ``VmafCudaState``
 *  into the ``VmafContext`` **by value**, so the batch the engine
 *  drives is the context's own copy, and its drain stream is destroyed
 *  with that copy in ``vmaf_close_backends``. The caller's original
 *  state keeps an untouched (zeroed) batch, so two contexts importing
 *  the same state never share batch storage.
 *
 *  Concurrency
 *  -----------
 *  A ``VmafCudaState`` is documented as not thread-safe (one state per
 *  driver thread — see ``libvmaf_cuda.h``), and the library's frame
 *  loop is single-threaded for the GPU dispatch path (the
 *  ``thread_pool`` parallelisation is for CPU extractors only — see
 *  ``read_pictures_should_skip``), so state-scoped storage needs no
 *  locking.
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

/* ``VMAF_CUDA_DRAIN_BATCH_MAX`` and ``VmafCudaDrainBatch`` are declared
 * in ``common.h`` — the batch is a member of ``VmafCudaState``. */

/**
 * Open a drain batch on @p cu_state.
 *
 * Idempotent: a second open without an intervening close is a no-op
 * and keeps the existing batch. NULL @p cu_state is a no-op.
 */
void vmaf_cuda_drain_batch_open(VmafCudaState *cu_state);

/**
 * Register an extractor lifecycle into @p cu_state's open batch.
 *
 * Called by template-based extractors at the end of submit() (after
 * ``cuEventRecord(lc->finished, lc->str)``). When no batch is open,
 * this is a no-op so individual ``submit()`` calls outside the
 * orchestrator (e.g. unit tests) keep working unchanged.
 *
 * Returns 0 on success, 0 also when the batch is closed (no-op),
 * -EINVAL for a NULL @p cu_state or @p lc, and -ENOSPC when the batch
 * is full (lifecycle skipped, the caller falls back to per-stream sync
 * via ``vmaf_cuda_kernel_collect_wait``).
 */
int vmaf_cuda_drain_batch_register(VmafCudaState *cu_state, struct VmafCudaKernelLifecycle *lc);

/**
 * Register a raw (event, drained-flag) pair into @p cu_state's batch.
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
 * The @p cu_state passed here must be the same state the extractor is
 * bound to (``fex->cu_state``), i.e. the context's own copy — see the
 * ownership note above.
 *
 * Returns 0 on success, 0 when the batch is closed (no-op), -EINVAL
 * for a NULL @p cu_state or @p drained_out, or -ENOSPC on overflow
 * (caller falls back to per-stream sync).
 */
int vmaf_cuda_drain_batch_register_event(VmafCudaState *cu_state, CUevent finished,
                                         bool *drained_out);

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
 * Returns 0 on success, -EINVAL for a NULL @p cu_state, or a negative
 * errno from ``vmaf_cuda_result_to_errno`` on the first CUDA failure.
 */
int vmaf_cuda_drain_batch_flush(VmafCudaState *cu_state);

/**
 * Close @p cu_state's drain batch.
 *
 * Resets the registration list *and* clears the registered event /
 * flag slots, so no handle owned by a torn-down extractor stays
 * reachable. Does not destroy the drain stream — that lives until
 * ``vmaf_cuda_drain_batch_destroy``. NULL @p cu_state is a no-op.
 */
void vmaf_cuda_drain_batch_close(VmafCudaState *cu_state);

/**
 * Tear down @p cu_state's drain stream and empty its batch.
 *
 * Called from the engine on context shutdown, before the CUcontext
 * itself is released. Safe on a state that never opened a batch, and
 * safe to call more than once. NULL @p cu_state is a no-op.
 */
void vmaf_cuda_drain_batch_destroy(VmafCudaState *cu_state);

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_CUDA_DRAIN_BATCH_H_ */
