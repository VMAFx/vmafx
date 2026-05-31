/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  CUDA fence-batching helpers (T-GPU-OPT-1, ADR-0242).
 *
 *  See ``drain_batch.h`` for the contract. This TU owns the
 *  thread-local registration list and the lazy-allocated shared
 *  drain stream.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "drain_batch.h"
#include "kernel_template.h"

/* Per-thread drain batch.
 *
 * - ``open``       : the engine entered submit-all mode.
 * - ``n``          : number of registered entries in this batch.
 * - ``finished[]`` : registered CUevent (lifecycle->finished or raw
 *                    legacy s->finished) waited on as a group.
 * - ``flags[]``    : pointer to the bool the extractor's collect()
 *                    polls to decide whether to skip its sync. For
 *                    template-based extractors this is
 *                    ``&lc->drained``; for legacy extractors it is
 *                    a per-state ``s->drained`` field.
 * - ``drain_str``  : lazily-created shared drain stream. Reused across
 *                    batches on the same thread; destroyed at engine
 *                    shutdown via ``vmaf_cuda_drain_batch_thread_destroy``.
 *
 * The library's GPU dispatch loop is single-threaded
 * (``read_pictures_extractor_loop`` runs serially for GPU paths —
 * the ``thread_pool`` only parallelises CPU extractors), so a TLS
 * batch matches the actual call graph 1:1. */
typedef struct DrainBatchTls {
    bool open;
    unsigned n;
    CUevent finished[VMAF_CUDA_DRAIN_BATCH_MAX];
    bool *flags[VMAF_CUDA_DRAIN_BATCH_MAX];
    CUstream drain_str;
} DrainBatchTls;

static _Thread_local DrainBatchTls g_drain_batch;

void vmaf_cuda_drain_batch_open(void)
{
    if (g_drain_batch.open) {
        return;
    }
    g_drain_batch.open = true;
    g_drain_batch.n = 0;
}

/* Shared registration body — both public entry points (lifecycle
 * and raw event+flag) feed into this. ``finished`` may be NULL only
 * if the caller has chosen not to wait on the event in this batch
 * (currently never — both call sites pass real events); the wait
 * step skips NULLs anyway as a defensive belt-and-braces guard. */
static int drain_batch_register_raw(CUevent finished, bool *drained_flag)
{
    if (!g_drain_batch.open) {
        /* No batch in progress — leave behaviour unchanged so the
         * extractor's own collect_wait does the per-stream sync. */
        return 0;
    }
    if (g_drain_batch.n >= VMAF_CUDA_DRAIN_BATCH_MAX) {
        /* Static cap reached — silently degrade to per-stream sync
         * for this entry. ADR-0242 § Failure mode. */
        return -ENOSPC;
    }
    g_drain_batch.finished[g_drain_batch.n] = finished;
    g_drain_batch.flags[g_drain_batch.n] = drained_flag;
    g_drain_batch.n++;
    return 0;
}

int vmaf_cuda_drain_batch_register(VmafCudaKernelLifecycle *lc)
{
    if (lc == NULL) {
        return -EINVAL;
    }
    return drain_batch_register_raw(lc->finished, &lc->drained);
}

int vmaf_cuda_drain_batch_register_event(CUevent finished, bool *drained_out)
{
    if (drained_out == NULL) {
        return -EINVAL;
    }
    return drain_batch_register_raw(finished, drained_out);
}

/* Lazily create the per-thread drain stream. The CUcontext must
 * already be active on the calling thread (the orchestrator pushes
 * it before entering the dispatch loop, then pops on the way out).
 * We do NOT push/pop here — that would re-enter the context and
 * race with the kernel-template helpers that assume a stable
 * push/pop pairing. */
static int drain_stream_ensure(VmafCudaState *cu_state)
{
    if (g_drain_batch.drain_str != NULL) {
        return 0;
    }
    CudaFunctions *cu_f = cu_state->f;
    int ctx_pushed = 0;
    int _cuda_err = 0;
    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    /* Non-blocking: the drain stream must not implicitly serialise
     * with the legacy NULL stream (matches the per-extractor stream
     * flag in ``vmaf_cuda_kernel_lifecycle_init``). */
    CHECK_CUDA_GOTO(cu_f,
                    cuStreamCreateWithPriority(&g_drain_batch.drain_str, CU_STREAM_NON_BLOCKING, 0),
                    fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;
fail:
    if (ctx_pushed) {
        const CUresult pop_res = cu_f->cuCtxPopCurrent(NULL);
        if (pop_res != CUDA_SUCCESS && _cuda_err == 0) {
            _cuda_err = vmaf_cuda_result_to_errno((int)pop_res);
        }
    }
fail_after_pop:
    g_drain_batch.drain_str = NULL;
    return _cuda_err;
}

int vmaf_cuda_drain_batch_flush(VmafCudaState *cu_state)
{
    if (cu_state == NULL) {
        return -EINVAL;
    }
    if (!g_drain_batch.open || g_drain_batch.n == 0U) {
        return 0;
    }

    /* Power-of-10 §5: invariants on the batch table at flush entry. */
    assert(g_drain_batch.n <= VMAF_CUDA_DRAIN_BATCH_MAX);
    assert(cu_state->f != NULL);

    int err = drain_stream_ensure(cu_state);
    if (err != 0) {
        return err;
    }
    CudaFunctions *cu_f = cu_state->f;
    assert(g_drain_batch.drain_str != NULL);

    /* Wait on every registered ``finished`` event from the shared
     * drain stream, then synchronize the drain stream once. After
     * cuStreamSynchronize returns, all events have completed →
     * every extractor's pinned host buffer is safe to read. */
    for (unsigned i = 0; i < g_drain_batch.n; i++) {
        CUevent ev = g_drain_batch.finished[i];
        if (ev == NULL) {
            continue;
        }
        CHECK_CUDA_RETURN(cu_f,
                          cuStreamWaitEvent(g_drain_batch.drain_str, ev, CU_EVENT_WAIT_DEFAULT));
    }
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(g_drain_batch.drain_str));

    /* Mark each registered entry drained so the matching collect()
     * skips its private cuStreamSynchronize. */
    for (unsigned i = 0; i < g_drain_batch.n; i++) {
        if (g_drain_batch.flags[i] != NULL) {
            *g_drain_batch.flags[i] = true;
        }
    }
    return 0;
}

void vmaf_cuda_drain_batch_close(void)
{
    g_drain_batch.open = false;
    g_drain_batch.n = 0;
    /* Note: per-entry ``drained`` flags are reset lazily by each
     * extractor's collect() — see kernel_template.h
     * ``vmaf_cuda_kernel_collect_wait`` and the legacy collect()
     * paths in integer_motion/adm/vif_cuda.c. */
}

void vmaf_cuda_drain_batch_thread_destroy(VmafCudaState *cu_state)
{
    if (cu_state == NULL || g_drain_batch.drain_str == NULL) {
        g_drain_batch.drain_str = NULL;
        return;
    }
    CudaFunctions *cu_f = cu_state->f;
    int ctx_pushed = 0;
    if (cu_f->cuCtxPushCurrent(cu_state->ctx) == CUDA_SUCCESS) {
        ctx_pushed = 1;
    }
    (void)cu_f->cuStreamSynchronize(g_drain_batch.drain_str);
    (void)cu_f->cuStreamDestroy(g_drain_batch.drain_str);
    g_drain_batch.drain_str = NULL;
    if (ctx_pushed) {
        (void)cu_f->cuCtxPopCurrent(NULL);
    }
}
