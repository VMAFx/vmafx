/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  CUDA fence-batching helpers (T-GPU-OPT-1, PR #312; re-scoped to the
 *  backend state by ADR-1187).
 *
 *  See ``drain_batch.h`` for the contract. The batch table and the
 *  lazily-allocated drain stream both live in the caller's
 *  ``VmafCudaState`` (``cu_state->drain_batch``) so their lifetime is
 *  the backend state's rather than the OS thread's — ADR-1187.
 */

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "drain_batch.h"
#include "kernel_template.h"

void vmaf_cuda_drain_batch_open(VmafCudaState *cu_state)
{
    if (cu_state == NULL) {
        return;
    }
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    if (b->open) {
        return;
    }
    b->open = true;
    b->n = 0;
}

/* Shared registration body — both public entry points (lifecycle
 * and raw event+flag) feed into this. ``finished`` may be NULL only
 * if the caller has chosen not to wait on the event in this batch
 * (currently never — both call sites pass real events); the wait
 * step skips NULLs anyway as a defensive belt-and-braces guard. */
static int drain_batch_register_raw(VmafCudaState *cu_state, CUevent finished, bool *drained_flag)
{
    if (cu_state == NULL) {
        return -EINVAL;
    }
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    if (!b->open) {
        /* No batch in progress — leave behaviour unchanged so the
         * extractor's own collect_wait does the per-stream sync. */
        return 0;
    }
    if (b->n >= VMAF_CUDA_DRAIN_BATCH_MAX) {
        /* Static cap reached — silently degrade to per-stream sync
         * for this entry. ``drain_batch.h`` § Failure mode. */
        return -ENOSPC;
    }
    b->finished[b->n] = finished;
    b->flags[b->n] = drained_flag;
    b->n++;
    return 0;
}

int vmaf_cuda_drain_batch_register(VmafCudaState *cu_state, VmafCudaKernelLifecycle *lc)
{
    if (lc == NULL) {
        return -EINVAL;
    }
    return drain_batch_register_raw(cu_state, lc->finished, &lc->drained);
}

int vmaf_cuda_drain_batch_register_event(VmafCudaState *cu_state, CUevent finished,
                                         bool *drained_out)
{
    if (drained_out == NULL) {
        return -EINVAL;
    }
    return drain_batch_register_raw(cu_state, finished, drained_out);
}

/* Lazily create the state's drain stream. The CUcontext must
 * already be active on the calling thread (the orchestrator pushes
 * it before entering the dispatch loop, then pops on the way out).
 * We do NOT push/pop here — that would re-enter the context and
 * race with the kernel-template helpers that assume a stable
 * push/pop pairing. */
static int drain_stream_ensure(VmafCudaState *cu_state)
{
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    if (b->drain_str != NULL) {
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
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&b->drain_str, CU_STREAM_NON_BLOCKING, 0),
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
    b->drain_str = NULL;
    return _cuda_err;
}

int vmaf_cuda_drain_batch_flush(VmafCudaState *cu_state)
{
    if (cu_state == NULL) {
        return -EINVAL;
    }
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    if (!b->open || b->n == 0U) {
        return 0;
    }

    /* Power-of-10 §5: invariants on the batch table at flush entry. */
    assert(b->n <= VMAF_CUDA_DRAIN_BATCH_MAX);
    assert(cu_state->f != NULL);

    int err = drain_stream_ensure(cu_state);
    if (err != 0) {
        return err;
    }
    CudaFunctions *cu_f = cu_state->f;
    assert(b->drain_str != NULL);

    /* Wait on every registered ``finished`` event from the shared
     * drain stream, then synchronize the drain stream once. After
     * cuStreamSynchronize returns, all events have completed →
     * every extractor's pinned host buffer is safe to read. */
    for (unsigned i = 0; i < b->n; i++) {
        CUevent ev = b->finished[i];
        if (ev == NULL) {
            continue;
        }
        CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(b->drain_str, ev, CU_EVENT_WAIT_DEFAULT));
    }
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(b->drain_str));

    /* Mark each registered entry drained so the matching collect()
     * skips its private cuStreamSynchronize. */
    for (unsigned i = 0; i < b->n; i++) {
        if (b->flags[i] != NULL) {
            *b->flags[i] = true;
        }
    }
    return 0;
}

void vmaf_cuda_drain_batch_close(VmafCudaState *cu_state)
{
    if (cu_state == NULL) {
        return;
    }
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    /* Drop the registered handles as well as the count. The entries alias
     * CUevents and `bool *`s owned by extractors that may be torn down
     * before this state is (vmaf_close destroys the extractor vector
     * first) — leaving them readable is what made the pre-ADR-1187
     * thread-local batch a use-after-free source. The drain stream is a
     * state-lifetime resource and is deliberately carried across the
     * reset; only vmaf_cuda_drain_batch_destroy releases it. */
    CUstream drain_str = b->drain_str;
    *b = (VmafCudaDrainBatch){0};
    b->drain_str = drain_str;
    /* Power-of-10 §5: post-conditions on the batch table at close exit. */
    assert(!b->open);
    assert(b->n == 0U);
    /* Note: per-entry ``drained`` flags are reset lazily by each
     * extractor's collect() — see kernel_template.h
     * ``vmaf_cuda_kernel_collect_wait`` and the legacy collect()
     * paths in integer_motion/adm/vif_cuda.c. */
}

void vmaf_cuda_drain_batch_destroy(VmafCudaState *cu_state)
{
    if (cu_state == NULL) {
        return;
    }
    VmafCudaDrainBatch *const b = &cu_state->drain_batch;
    CUstream drain_str = b->drain_str;
    /* Empty the batch unconditionally: an abandoned frame loop reaches
     * teardown with `open == true` and `n > 0`, and those entries name
     * extractor-owned handles that are about to be (or have already been)
     * freed. ADR-1187. */
    *b = (VmafCudaDrainBatch){0};
    assert(!b->open);
    assert(b->n == 0U);
    if (drain_str == NULL) {
        return;
    }
    CudaFunctions *cu_f = cu_state->f;
    assert(cu_f != NULL);
    int ctx_pushed = 0;
    if (cu_f->cuCtxPushCurrent(cu_state->ctx) == CUDA_SUCCESS) {
        ctx_pushed = 1;
    }
    (void)cu_f->cuStreamSynchronize(drain_str);
    (void)cu_f->cuStreamDestroy(drain_str);
    if (ctx_pushed) {
        (void)cu_f->cuCtxPopCurrent(NULL);
    }
    /* Post-condition: drain stream is always cleared. */
    assert(b->drain_str == NULL);
}
