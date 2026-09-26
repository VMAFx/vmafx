/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
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
 *          → vmaf_cuda_kernel_readback_free (device accumulator +
 *            pinned host slot via vmaf_cuda_buffer_host_free)
 *
 *  The helpers here own the lifecycle pieces that DON'T differ per
 *  metric (stream + event + accumulator triple). Per-metric work
 *  (kernel launch params, host-side reduction, score-emit) stays in
 *  the calling TU — that's where the metric-specific math lives.
 *
 *  Reference implementation: core/src/feature/cuda/integer_psnr_cuda.c.
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
 * back any partial state. Handles destroyed successfully are cleared;
 * a handle whose destroy call fails is retained for a later close retry.
 */
/* NOLINTBEGIN(modernize-use-nullptr): C header. The fork builds C as C23,
 * where clang-tidy proposes `nullptr`, but MSVC's documented /std:clatest
 * feature set does not include it while the required Windows build compiles
 * the CUDA host TUs with cl.exe. ADR-1138. */
static inline void vmaf_cuda_kernel_lifecycle_init_unwind(VmafCudaKernelLifecycle *lc,
                                                          VmafCudaState *cu_state, int ctx_pushed)
{
    /* No work can have been submitted before init returns, so all partially
     * created handles are independent and can be released best-effort. Each
     * helper retains a handle whose driver release fails for a later close
     * retry while preserving the original init error at the call site. */
    (void)vmaf_cuda_stream_destroy(cu_state, &lc->str, false);
    (void)vmaf_cuda_event_destroy(cu_state, &lc->submit);
    (void)vmaf_cuda_event_destroy(cu_state, &lc->finished);
    if (ctx_pushed != 0)
        (void)cu_state->f->cuCtxPopCurrent(NULL);
}

static inline int vmaf_cuda_kernel_lifecycle_init(VmafCudaKernelLifecycle *lc,
                                                  VmafCudaState *cu_state)
{
    if (lc == NULL || cu_state == NULL || cu_state->f == NULL || cu_state->ctx == NULL)
        return -EINVAL;

    CudaFunctions *cu_f = cu_state->f;
    int _cuda_err = 0;
    int ctx_pushed = 0;

    CHECK_CUDA_GOTO(cu_f, cuCtxPushCurrent(cu_state->ctx), fail);
    ctx_pushed = 1;
    CHECK_CUDA_GOTO(cu_f, cuStreamCreateWithPriority(&lc->str, CU_STREAM_NON_BLOCKING, 0), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&lc->submit, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuEventCreate(&lc->finished, CU_EVENT_DEFAULT), fail);
    CHECK_CUDA_GOTO(cu_f, cuCtxPopCurrent(NULL), fail);
    return 0;

fail:
    vmaf_cuda_kernel_lifecycle_init_unwind(lc, cu_state, ctx_pushed);
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
    (void)lc;
    CudaFunctions *cu_f = cu_state->f;
    /* The zeroing MUST be issued on `picture_stream`, the same stream the
     * kernel launches on.
     *
     * It used to go to `lc->str`, the extractor's private readback stream.
     * Nothing ordered the two: CUDA only guarantees ordering within a stream,
     * so the memset on `lc->str` and the accumulating kernel on
     * `picture_stream` could overlap in either direction. When the memset
     * landed after some atomic adds had already run it erased them, and the
     * feature reported a sum that was too LOW — the CPU/CUDA parity tests saw
     * e.g. `cpu=127.50000000 cuda=120.87500000`. It reproduced only on a
     * loaded GPU (the full 203-test suite at -j32), roughly one run in three,
     * and never standalone, which is why it read as flakiness rather than as
     * the race it is.
     *
     * Issuing it here lets program order on a single stream do the work:
     * memset, then kernel. The readback stays fenced separately by
     * `lc->submit`. */
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(rb->device->data, 0, rb->bytes, picture_stream));
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
    /* Invalidate any drain left over from an earlier frame before recording
     * this frame's fence. `lc->drained` tells `vmaf_cuda_kernel_collect_wait`
     * it may skip its `cuStreamSynchronize`; it is set by a batch flush and
     * cleared only by a matching `collect()`, and
     * `vmaf_cuda_drain_batch_close()` clears the batch table but not the
     * per-entry flags. A flag can therefore outlive its frame whenever a
     * registered extractor's `collect()` does not run, letting the next
     * frame's collect skip a sync it still needs. Clearing here makes the flag
     * mean exactly "a flush completed since this submit". */
    lc->drained = false;
    CHECK_CUDA_RETURN(cu_f, cuEventRecord(lc->finished, lc->str));
    /* Best-effort: drain-batch registration failure (overflow, no
     * batch open) silently degrades to per-stream sync; never
     * propagated as an error — the extractor is still correct. */
    (void)vmaf_cuda_drain_batch_register(lc);
    return 0;
}

/*
 * close()-side teardown: drain + destroy stream, then destroy events.
 *
 * Phased: DRAIN STREAM FIRST. If sync or stream destroy fails, the stream
 * is still live and events may be in-flight, so event destruction is
 * skipped entirely. Only after quiescence (stream successfully destroyed)
 * are events released. Module unloading is the caller's responsibility
 * after lifecycle close returns (it depends on the context push, not
 * the stream).
 *
 * Returns the first negative errno encountered (or 0). On stream-phase
 * failure the returned error indicates the stream and events are still
 * alive and retryable.
 *
 * Safe to call on a partially-initialised lifecycle (handles that
 * are NULL/0 are skipped).
 */
static inline int vmaf_cuda_kernel_lifecycle_close(VmafCudaKernelLifecycle *lc,
                                                   VmafCudaState *cu_state)
{
    if (lc == NULL || cu_state == NULL || cu_state->f == NULL || cu_state->ctx == NULL)
        return -EINVAL;
    if (lc->str == NULL && lc->submit == NULL && lc->finished == NULL) {
        lc->drained = false;
        return 0;
    }

    /* Phase 1: drain and destroy the stream. */
    int rc = vmaf_cuda_stream_destroy(cu_state, &lc->str, true);

    /* Phase 2: destroy events only after stream quiescence.
     * If the stream is still live, events may be in-flight; destroying
     * them while the stream references them is undefined. */
    if (lc->str == NULL) {
        const int submit_err = vmaf_cuda_event_destroy(cu_state, &lc->submit);
        if (submit_err != 0 && rc == 0)
            rc = submit_err;
        const int finished_err = vmaf_cuda_event_destroy(cu_state, &lc->finished);
        if (finished_err != 0 && rc == 0)
            rc = finished_err;
    }

    lc->drained = false;
    return rc;
}
/*
 * Free the readback pair. Mirrors vmaf_cuda_kernel_readback_alloc's
 * leave-partial-state-on-failure contract: this routine is safe to
 * call on a partially-allocated readback.
 *
 * Both the device accumulator and the pinned host buffer are released
 * here.  The owned helpers null the caller's pointer only when the
 * underlying driver free succeeds, so a failed free retains ownership
 * for retry.  Uses first-error preservation.
 */
static inline int vmaf_cuda_kernel_readback_free(VmafCudaKernelReadback *rb,
                                                 VmafCudaState *cu_state)
{
    int rc = 0;
    if (rb->device != NULL) {
        const int e = vmaf_cuda_buffer_free_owned(cu_state, &rb->device);
        if (e != 0 && rc == 0)
            rc = e;
    }
    if (rb->host_pinned != NULL) {
        const int e = vmaf_cuda_buffer_host_free_owned(cu_state, &rb->host_pinned);
        if (e != 0 && rc == 0)
            rc = e;
    }
    if (rb->device == NULL && rb->host_pinned == NULL)
        rb->bytes = 0;
    return rc;
}
/* NOLINTEND(modernize-use-nullptr) */

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* LIBVMAF_CUDA_KERNEL_TEMPLATE_H_ */
