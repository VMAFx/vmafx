/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 */

#ifndef LIBVMAF_HIP_PICTURE_HIP_H_
#define LIBVMAF_HIP_PICTURE_HIP_H_

#include <stddef.h>
#include <stdint.h>

#include "common.h"
#include "libvmaf/picture.h"

/*
 * Uploading a host picture: vmaf_hip_picture_upload().
 *
 * HIP pictures are pageable host memory, and hipMemcpy2DAsync is documented
 * as asynchronous with respect to the host: it can return while the copy is
 * still reading its source. A HIP extractor's submit() hands the picture
 * back to the caller when it returns, and the CLI's picture pool refills it
 * with the next frame at once, so an upload still in flight scores the frame
 * against the next frame's samples. An extractor stages its pictures through
 * this helper so that it never returns from submit() with an upload in
 * flight (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
 *
 * The wait is an event recorded on `stream` after the copies, not a stream
 * synchronisation: on the null stream (`stream == 0`) a stream
 * synchronisation also waits for every other stream on the device, the
 * other extractors' kernels included. Only the copies, and whatever was
 * already queued ahead of them on `stream`, are waited for. Pass the
 * extractor's private stream (`VmafHipKernelLifecycle::str`), which
 * collect() has drained, not the null stream: a null-stream copy queues
 * behind every null-stream kernel of the frame by definition.
 *
 * Cost: the host blocks until the copy has run, where it used to run ahead
 * of the GPU. On a device with one hardware queue the copy cannot start
 * until the previous extractor's kernels leave the GPU: on the gfx1036 iGPU
 * float_motion_hip waited 14 ms a frame behind float_adm_hip at 1080p, and
 * vmaf_float_v0.6.1 lost 21 % of its throughput (vmaf_v0.6.1 and the single
 * extractors stayed within noise, vif_hip lost 7 %). Running the kernels on
 * the private stream too made no difference. Staging through
 * extractor-owned pinned buffers removes the wait:
 * vmaf_hip_picture_upload_staged() below, adopted extractor by extractor
 * (T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19 in docs/state.md).
 */

/*
 * Uploading without a host wait: vmaf_hip_picture_upload_staged().
 *
 * The host copies every plane into `staging`, pinned host memory the
 * extractor owns (vmaf_hip_picture_staging_alloc()), before the call returns,
 * so the caller may recycle the pictures at once; the device copies then run
 * from `staging`, in stream order, while the host goes on. Nothing waits.
 *
 * `staging` must not be rewritten before those copies have run. An extractor
 * that stages once per frame gets that from the frame protocol: its
 * collect() drains the stream the copies ran on, and libvmaf collects frame
 * N - 1 before it submits frame N (dispatch_gpu_double_buffer()), so the
 * next submit() finds the copies done. Close drains the stream before it
 * frees the buffer. ADR-1377.
 */

/*
 * One host picture plane to copy into a device buffer with
 * vmaf_hip_picture_upload(): `rows` rows of `row_bytes` bytes, read from
 * `pic->data[plane]` `pic->stride[plane]` bytes apart and written to `dst`
 * `dst_pitch` bytes apart.
 */
typedef struct VmafHipPlaneUpload {
    void *dst;
    size_t dst_pitch;
    const VmafPicture *pic;
    unsigned plane;
    size_t row_bytes;
    size_t rows;
} VmafHipPlaneUpload;

#ifdef __cplusplus
extern "C" {
#endif

int vmaf_hip_picture_alloc(VmafHipContext *ctx, void **out, size_t size);
void vmaf_hip_picture_free(VmafHipContext *ctx, void *buf);

/*
 * Copy host picture planes to the device on `stream`, and return only once
 * every copy has finished reading its host plane; see the note above.
 *
 * `stream` is a `hipStream_t` carried as `uintptr_t`, the kernel-template
 * convention (ADR-0241). Returns 0 or a negative errno; when a copy fails
 * to enqueue, the copies already enqueued are still waited for before the
 * error is returned.
 */
int vmaf_hip_picture_upload(const VmafHipPlaneUpload *planes, unsigned n_planes, uintptr_t stream);

/*
 * Copy host picture planes into `staging`, packed plane after plane
 * (`row_bytes` apart), then enqueue their device copies on `stream` and
 * return without waiting; see the note above. `staging_bytes` must cover the
 * sum of `rows * row_bytes` over `planes`. Returns 0 or a negative errno;
 * when a copy fails to enqueue after earlier ones were, it waits for those
 * before returning (error path only), so `staging` is free again.
 */
int vmaf_hip_picture_upload_staged(const VmafHipPlaneUpload *planes, unsigned n_planes,
                                   void *staging, size_t staging_bytes, uintptr_t stream);

/* Pinned host staging for vmaf_hip_picture_upload_staged(), `size` bytes.
 * Free it with vmaf_hip_picture_staging_free() once the stream that copies
 * from it is drained; freeing NULL is a no-op. */
int vmaf_hip_picture_staging_alloc(void **out, size_t size);
void vmaf_hip_picture_staging_free(void *staging);

#ifdef __cplusplus
}
#endif

#endif /* LIBVMAF_HIP_PICTURE_HIP_H_ */
