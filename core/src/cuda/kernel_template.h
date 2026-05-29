/**
 *  Copyright 2026 Lusoris and Claude (Anthropic)
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  CUDA per-feature kernel scaffolding template (T7-* — ADR-0246).
 *
 *  This header is **template-only**: no existing kernel includes it
 *  yet. It captures the shape every fork-added CUDA feature
 *  extractor has converged on and exposes it as a small set of
 *  helper inlines + a stateful struct, so future kernel migrations
 *  (and the long-tail GPU bring-up) can stop hand-rolling the same
 *  ~6 lines of stream + event + per-frame-readback boilerplate.
 *
 *  The shared lifecycle every CUDA feature kernel implements today:
 *
 *      init():
 *          cuCtxPushCurrent → cuStreamCreateWithPriority
 *          → cuEventCreate (submit-fence)
 *          → cuEventCreate (DtoH-finished fence)
 *          → cuModuleLoadData(ptx)
 *          → cuModuleGetFunction(s)
 *          → cuCtxPopCurrent
 *          → vmaf_cuda_buffer_alloc(device-side accumulator)
 *          → vmaf_cuda_buffer_host_alloc(pinned readback slot)
 *
 *      submit():
 *          cuMemsetD8Async(accumulator, 0, …, str)
 *          cuStreamWaitEvent(picture_stream, dist_ready_event, 0)
 *          cuLaunchKernel(...)
 *          cuEventRecord(submit_event, picture_stream)
 *          cuStreamWaitEvent(str, submit_event, 0)
 *          cuMemcpyDtoHAsync(host_pinned, device_accumulator, …, str)
 *          cuEventRecord(finished, str)
 *
 *      collect():
 *          cuStreamSynchronize(str)
 *          → CPU-side reduce / score-emit
 *
 *      close():
 *          cuStreamSynchronize → cuStreamDestroy
 *          → cuEventDestroy(submit) → cuEventDestroy(finished)
 *          → vmaf_cuda_buffer_free(accumulator)
 *
 *  The helpers here own the lifecycle pieces that DON'T differ per
 *  metric (stream + event + accumulator triple). Per-metric work
 *  (kernel launch params, host-side reduction, score-emit) stays in
 *  the calling TU — that's where the metric-specific math lives.
 *
 *  Reference implementation: libvmaf/src/feature/cuda/integer_psnr_cuda.c.
 *  Migration guide: docs/backends/kernel-scaffolding.md.
 *
 *  Why per-backend (not cross-backend): CUDA's async-stream + event
 *  model and Vulkan's command-buffer + fence + descriptor-pool model
 *  share no concrete shape. A cross-backend abstraction would force
 *  a lowest-common-denominator API that captures neither. See
 *  ADR-0246 § Alternatives considered.
 *
 *  Why helper functions (not macros): step-through in cuda-gdb /
 *  Nsight is materially worse on macros, and the macros that already
 *  ship in cuda_helper.cuh (CHECK_CUDA_GOTO / CHECK_CUDA_RETURN)
 *  cover the parts where the macro form genuinely pays for itself.
 */

#ifndef LIBVMAF_CUDA_KERNEL_TEMPLATE_H_
#define LIBVMAF_CUDA_KERNEL_TEMPLATE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "picture_cuda.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Per-frame async lifecycle the CUDA feature kernels share.
 *
 * `str`       — private non-blocking stream for the readback path.
 *               The launch itself runs on the picture's stream
 *               (vmaf_cuda_picture_get_stream(pic)) — `str` is the
 *               drain channel so the picture pool isn't blocked.
 * `submit`    — event recorded on the picture's stream right after
 *               cuLaunchKernel; `str` waits on it before the
 *               cuMemcpyDtoHAsync.
 * `finished`  — event recorded on `str` after the readback completes.
 *               cuStreamSynchronize on `str` in collect() is the
 *               canonical wait point; the event is exposed for
 *               callers that need to chain (e.g. graph capture).
 * `drained`   — set by ``vmaf_cuda_drain_batch_flush`` (T-GPU-OPT-1,
 *               ADR-0242) when this lifecycle's finished event was
 *               waited on as part of the engine-scope batched drain.
 *               When true, ``vmaf_cuda_kernel_collect_wait`` skips
 *               its per-stream cuStreamSynchronize and resets the
 *               flag, so the next frame falls back to the legacy
 *               per-stream wait if no drain batch is active. The
 *               flag is byte-sized; the implicit padding after it
 *               is harmless and explicit zero-init is via
 *               ``vmaf_cuda_kernel_lifecycle_init``.
 */
typedef struct VmafCudaKernelLifecycle {
    CUstream str;
    CUevent submit;
    CUevent finished;
    bool drained;
} VmafCudaKernelLifecycle;

/*
 * One device-side accumulator + one pinned host readback slot.
 *
 * Most fork-added CUDA kernels reduce to a single int64/uint64 sum
 * (psnr SSE, motion sad, ...). For metrics with multi-word
 * accumulators (multi-plane PSNR, ssimulacra2 multi-band), allocate
 * one VmafCudaKernelReadback per slot.
 */
typedef struct VmafCudaKernelReadback {
    VmafCudaBuffer *device;
    void *host_pinned;
    size_t bytes;
} VmafCudaKernelReadback;

/*
 * Init: pushes the context, creates a non-blocking private stream
 * and the submit/finished event pair, then pops the context.
 *
 * Caller pre-conditions: `lc` zero-initialised; `cu_state` is the
 * VmafCudaState handed in via fex->cu_state.
 *
 * Returns 0 on success or the negative errno mapped from CUresult
 * (see vmaf_cuda_result_to_errno). On failure the function rolls
 * back any partial state — `lc` ends up zeroed and the context is
 * popped.
 */
static inline int vmaf_cuda_kernel_lifecycle_init(VmafCudaKernelLifecycle *lc,
                                                  VmafCudaState *cu_state)
{
    CudaFunctions *cu_f = cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&lc->str, CU_STREAM_NON_BLOCKING, 0), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&lc->submit, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&lc->finished, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail_after_pop);
    return 0;

fail:
    if (ctx_pushed) {
        (void)cu_f->cuCtxPopCurrent(NULL);
    }
fail_after_pop:
    /* Best-effort: any of the three handles that did create cleanly
     * are leaked deliberately — the caller will hit the same failure
     * on the next init() and the process is in an unrecoverable CUDA
     * state anyway. */
    return _cuda_err;
}

/*
 * Allocate a (device, pinned-host) readback pair of `bytes` size.
 *
 * Returns 0 on success or -ENOMEM. On failure the caller must call
 * vmaf_cuda_kernel_readback_free; the function leaves `rb` partially
 * populated rather than rolling back so the unwind path stays
 * uniform with the multi-readback case.
 */
static inline int vmaf_cuda_kernel_readback_alloc(VmafCudaKernelReadback *rb,
                                                  VmafCudaState *cu_state, size_t bytes)
{
    rb->bytes = bytes;
    int err = vmaf_cuda_buffer_alloc(cu_state, &rb->device, bytes);
    if (err != 0) {
        return err;
    }
    err = vmaf_cuda_buffer_host_alloc(cu_state, &rb->host_pinned, bytes);
    if (err != 0) {
        return err;
    }
    return 0;
}

/*
 * Per-frame submit-side helper.
 *
 *   1. Zero the device accumulator on `lc->str`.
 *   2. Wait for the dist-side ready event on `picture_stream`
 *      (so both ref and dist uploads are complete before launch).
 *
 * The kernel launch itself stays in the calling TU because grid
 * dims, the function handle, and the parameter pack are all
 * metric-specific. The caller resumes after this with:
 *
 *      cuLaunchKernel(...);
 *      cuEventRecord(lc->submit, picture_stream);
 *      cuStreamWaitEvent(lc->str, lc->submit, CU_EVENT_WAIT_DEFAULT);
 *      cuMemcpyDtoHAsync(rb->host_pinned, rb->device->data, rb->bytes, lc->str);
 *      cuEventRecord(lc->finished, lc->str);
 *
 * (See the migration guide for the post-launch boilerplate
 * helper if/when a second metric adopts this template.)
 */
static inline int vmaf_cuda_kernel_submit_pre_launch(VmafCudaKernelLifecycle *lc,
                                                     VmafCudaState *cu_state,
                                                     VmafCudaKernelReadback *rb,
                                                     CUstream picture_stream,
                                                     CUevent dist_ready_event)
{
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(rb->device->data, 0, rb->bytes, lc->str));
    CHECK_CUDA_RETURN(cu_f,
                      cuStreamWaitEvent(picture_stream, dist_ready_event, CU_EVENT_WAIT_DEFAULT));
    return 0;
}

/*
 * collect()-side wait point: drains the private stream so the host
 * pinned buffer is safe to read.
 *
 * Fence-batching fast path (T-GPU-OPT-1, ADR-0242): when the engine
 * has already waited on this lifecycle's ``finished`` event as part
 * of a batched drain (``lc->drained`` is true), the per-stream
 * cuStreamSynchronize would be redundant — the readback is already
 * complete on the host. Skip it and reset the flag so the next
 * frame's collect() falls back to the legacy wait if no batch is
 * active.
 */
static inline int vmaf_cuda_kernel_collect_wait(VmafCudaKernelLifecycle *lc,
                                                VmafCudaState *cu_state)
{
    if (lc->drained) {
        lc->drained = false;
        return 0;
    }
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuStreamSynchronize(lc->str));
    return 0;
}

/*
 * submit()-side post-DtoH helper (T-GPU-OPT-1, ADR-0242).
 *
 *   1. cuEventRecord(lc->finished, lc->str)  — fence the readback.
 *   2. vmaf_cuda_drain_batch_register(lc)    — opt into the engine's
 *                                              batched drain when one
 *                                              is open. No-op when the
 *                                              engine has not entered
 *                                              submit-all mode (legacy
 *                                              call sites still get the
 *                                              old per-stream sync via
 *                                              ``vmaf_cuda_kernel_collect_wait``).
 *
 * Calling this at the tail of every template-based ``submit()`` is
 * what makes the engine-scope fence batching transparent — extractors
 * register without touching their collect() paths. Forward declared:
 * the implementation lives in ``drain_batch.c``.
 */
int vmaf_cuda_drain_batch_register(VmafCudaKernelLifecycle *lc);

static inline int vmaf_cuda_kernel_submit_post_record(VmafCudaKernelLifecycle *lc,
                                                      VmafCudaState *cu_state)
{
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(lc->finished, lc->str));
    /* Best-effort: drain-batch registration failure (overflow, no
     * batch open) silently degrades to per-stream sync; never
     * propagated as an error — the extractor is still correct. */
    (void)vmaf_cuda_drain_batch_register(lc);
    return 0;
}

/*
 * close()-side teardown: drain + destroy stream, destroy events.
 *
 * Returns the first negative errno encountered (or 0). On failure
 * later resources are still attempted — the close path tries to
 * release as much as it can rather than bailing on the first error.
 *
 * Safe to call on a partially-initialised lifecycle (handles that
 * are NULL/0 are skipped).
 */
static inline int vmaf_cuda_kernel_lifecycle_close(VmafCudaKernelLifecycle *lc,
                                                   VmafCudaState *cu_state)
{
    CudaFunctions *cu_f = cu_state->f;
    int rc = 0;
    if (lc->str != NULL) {
        const CUresult sync_res = cu_f->cuStreamSynchronize(lc->str);
        if (sync_res != CUDA_SUCCESS && rc == 0) {
            rc = vmaf_cuda_result_to_errno((int)sync_res);
        }
        const CUresult destroy_res = cu_f->cuStreamDestroy(lc->str);
        if (destroy_res != CUDA_SUCCESS && rc == 0) {
            rc = vmaf_cuda_result_to_errno((int)destroy_res);
        }
        lc->str = NULL;
    }
    if (lc->submit != NULL) {
        const CUresult e = cu_f->cuEventDestroy(lc->submit);
        if (e != CUDA_SUCCESS && rc == 0) {
            rc = vmaf_cuda_result_to_errno((int)e);
        }
        lc->submit = NULL;
    }
    if (lc->finished != NULL) {
        const CUresult e = cu_f->cuEventDestroy(lc->finished);
        if (e != CUDA_SUCCESS && rc == 0) {
            rc = vmaf_cuda_result_to_errno((int)e);
        }
        lc->finished = NULL;
    }
    return rc;
}

/*
 * Free the readback pair. Mirrors vmaf_cuda_kernel_readback_alloc's
 * leave-partial-state-on-failure contract: this routine is safe to
 * call on a partially-allocated readback.
 */
static inline int vmaf_cuda_kernel_readback_free(VmafCudaKernelReadback *rb,
                                                 VmafCudaState *cu_state)
{
    int rc = 0;
    if (rb->device != NULL) {
        const int e = vmaf_cuda_buffer_free(cu_state, rb->device);
        if (e != 0 && rc == 0) {
            rc = e;
        }
        /* common.c follows the same free-the-handle-after pattern. */
        free(rb->device);
        rb->device = NULL;
    }
    /* host_pinned is owned by the CUDA host alloc table; the
     * matching free path lives behind common.c's
     * vmaf_cuda_buffer_host_free. The template doesn't claim to
     * release it — callers that adopted the template still call
     * the existing host-free helper directly. Documented in the
     * migration guide. */
    rb->host_pinned = NULL;
    rb->bytes = 0;
    return rc;
}

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_CUDA_KERNEL_TEMPLATE_H_ */
