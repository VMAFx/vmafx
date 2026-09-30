/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Picture allocation / lifecycle for the HIP backend (ADR-0212 / T7-10).
 *
 *  Implements a single-allocation device-memory pool per call.  The API
 *  deliberately mirrors `vmaf_cuda_picture_alloc` / `vmaf_cuda_picture_free`
 *  from `core/src/cuda/picture_cuda.c` so the two backends share the same
 *  conceptual model.
 *
 *  A pitched allocation (`hipMallocPitch`) would minimise bandwidth on tiled
 *  hardware, but the HIP picture pool follows the simpler `hipMalloc` (flat,
 *  row-major) path for its first non-stub revision (ADR-0613) because:
 *    1. All current callers use explicit `hipMemcpy*` rather than reading
 *       `pic->stride`, so pitch freedom buys nothing today.
 *    2. `hipMallocPitch` requires `width_bytes` + `height` per plane, but
 *       `vmaf_hip_picture_alloc` receives only a flat `size` byte count —
 *       changing the signature would require touching all 9 extractor sites.
 *  A full pitched-pool follow-up is tracked as T7-10c.
 *
 *  ADR-0613: fix P1-2 from the scaffold audit (core/src/hip/picture_hip.c
 *  previously returned -ENOSYS, blocking zero-copy upload for all HIP
 *  extractors).
 *
 *  vmaf_hip_picture_upload() is how a HIP extractor stages a host
 *  VmafPicture onto the device: it returns only once the copies have
 *  finished reading the picture (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
 *  vmaf_hip_picture_upload_staged() reads the picture into an
 *  extractor-owned pinned buffer instead and returns without waiting for the
 *  device copies (ADR-1377, T-HIP-UPLOAD-WAIT-THROUGHPUT-2026-09-19).
 */

#include <stddef.h>

#include "picture_hip.h"

/* `hip/meson.build` compiles this file whenever enable_hip is on. Without
 * enable_hipcc (HAVE_HIPCC) no extractor has a device kernel to feed, so
 * the entry points below return -ENOSYS instead. */
#ifdef HAVE_HIPCC
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <hip/hip_runtime_api.h>

#include "hip_handle.h"

/* Translate a HIP error to a negative POSIX errno.  Mirrors the static
 * helper present in every feature/hip/ extractor.  Every other error,
 * hipErrorNotInitialized and hipErrorDeinitialized included, is -EIO. */
static int hip_pic_rc_to_errno(hipError_t rc)
{
    switch (rc) {
    case hipSuccess:
        return 0;
    case hipErrorInvalidValue:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNoDevice:
        return -ENODEV;
    default:
        return -EIO;
    }
}

static bool hip_pic_plane_valid(const VmafHipPlaneUpload *p)
{
    if (p->dst == NULL || p->pic == NULL || p->plane >= 3u)
        return false;
    if (p->row_bytes == 0u || p->rows == 0u || p->dst_pitch < p->row_bytes)
        return false;
    const ptrdiff_t stride = p->pic->stride[p->plane];
    return p->pic->data[p->plane] != NULL && stride > 0 && (size_t)stride >= p->row_bytes;
}

/* Enqueue the copies in order, stopping at the first failure. `*enqueued`
 * counts the ones that were accepted, which the caller must still wait for. */
static hipError_t hip_pic_enqueue(const VmafHipPlaneUpload *planes, unsigned n_planes,
                                  hipStream_t str, unsigned *enqueued)
{
    hipError_t rc = hipSuccess;
    *enqueued = 0u;
    for (unsigned i = 0u; i < n_planes && rc == hipSuccess; i++) {
        const VmafHipPlaneUpload *p = &planes[i];
        rc = hipMemcpy2DAsync(p->dst, p->dst_pitch, p->pic->data[p->plane],
                              (size_t)p->pic->stride[p->plane], p->row_bytes, p->rows,
                              hipMemcpyHostToDevice, str);
        if (rc == hipSuccess)
            (*enqueued)++;
    }
    return rc;
}

/* Block until everything enqueued on `str` so far, the copies included, has
 * run. An event, so that the null stream waits for itself only. */
static hipError_t hip_pic_wait(hipEvent_t done, hipStream_t str)
{
    hipError_t rc = hipEventRecord(done, str);
    if (rc == hipSuccess)
        return hipEventSynchronize(done);
    /* No event to wait on: fall back to the whole stream, which is at
     * least as strong. */
    (void)hipStreamSynchronize(str);
    return rc;
}

int vmaf_hip_picture_upload(const VmafHipPlaneUpload *planes, unsigned n_planes, uintptr_t stream)
{
    if (planes == NULL || n_planes == 0u)
        return -EINVAL;
    /* Reject a bad plane before anything is enqueued, so nothing is left
     * reading a picture on this early return. */
    for (unsigned i = 0u; i < n_planes; i++) {
        if (!hip_pic_plane_valid(&planes[i]))
            return -EINVAL;
    }

    /* C translation unit, built by cl.exe on Windows, whose C23 has no
     * `nullptr` (ADR-1138). */
    // NOLINTNEXTLINE(modernize-use-nullptr): see the ADR-1138 note above.
    hipEvent_t done = NULL;
    hipError_t rc = hipEventCreateWithFlags(&done, hipEventDisableTiming);
    if (rc != hipSuccess)
        return hip_pic_rc_to_errno(rc);

    hipStream_t str = vmaf_hip_stream_of(stream);
    unsigned enqueued = 0u;
    const hipError_t copy_rc = hip_pic_enqueue(planes, n_planes, str, &enqueued);
    /* The wait below is only sufficient if every accepted copy was counted:
     * success means all of them, and a failure stops the loop early. */
    assert(enqueued <= n_planes);
    assert(copy_rc != hipSuccess || enqueued == n_planes);
    /* Wait even when a copy failed: the ones already enqueued still read the
     * pictures, and the caller may recycle them as soon as this returns. */
    rc = (enqueued > 0u) ? hip_pic_wait(done, str) : hipSuccess;
    (void)hipEventDestroy(done);
    if (copy_rc != hipSuccess)
        return hip_pic_rc_to_errno(copy_rc);
    return hip_pic_rc_to_errno(rc);
}

/* Bytes `planes` take packed in a staging buffer, or 0 when a plane is
 * invalid or the sum does not fit in size_t. */
static size_t hip_pic_staged_bytes(const VmafHipPlaneUpload *planes, unsigned n_planes)
{
    size_t total = 0u;
    for (unsigned i = 0u; i < n_planes; i++) {
        const VmafHipPlaneUpload *p = &planes[i];
        if (!hip_pic_plane_valid(p) || p->rows > SIZE_MAX / p->row_bytes)
            return 0u;
        const size_t bytes = p->rows * p->row_bytes;
        if (bytes > SIZE_MAX - total)
            return 0u;
        total += bytes;
    }
    return total;
}

/* Host copy of one plane into `dst`, rows packed `row_bytes` apart. Reads the
 * picture completely before it returns. */
static void hip_pic_stage_plane(const VmafHipPlaneUpload *p, uint8_t *dst)
{
    const uint8_t *src = (const uint8_t *)p->pic->data[p->plane];
    const size_t stride = (size_t)p->pic->stride[p->plane];
    for (size_t row = 0u; row < p->rows; row++)
        memcpy(dst + (row * p->row_bytes), src + (row * stride), p->row_bytes);
}

/* Error path only: copies already enqueued still read `staging`; wait for
 * them before the caller may rewrite it, then report the enqueue failure. */
static int hip_pic_drain_after_error(hipStream_t str, hipError_t failed)
{
    const hipError_t rc = hipStreamSynchronize(str);
    (void)rc; /* `failed` is the error to report */
    return hip_pic_rc_to_errno(failed);
}

int vmaf_hip_picture_upload_staged(const VmafHipPlaneUpload *planes, unsigned n_planes,
                                   void *staging, size_t staging_bytes, uintptr_t stream)
{
    if (planes == NULL || n_planes == 0u || staging == NULL)
        return -EINVAL;
    /* Validate everything before the first copy, so a rejected call leaves
     * nothing enqueued. */
    const size_t needed = hip_pic_staged_bytes(planes, n_planes);
    if (needed == 0u || needed > staging_bytes)
        return -EINVAL;

    hipStream_t str = vmaf_hip_stream_of(stream);
    uint8_t *at = (uint8_t *)staging;
    for (unsigned i = 0u; i < n_planes; i++) {
        const VmafHipPlaneUpload *p = &planes[i];
        hip_pic_stage_plane(p, at);
        /* From pinned memory the copy is asynchronous for real; the source
         * is the extractor's buffer, not the picture, so nothing waits. */
        const hipError_t rc = hipMemcpy2DAsync(p->dst, p->dst_pitch, at, p->row_bytes, p->row_bytes,
                                               p->rows, hipMemcpyHostToDevice, str);
        if (rc != hipSuccess && i > 0u)
            return hip_pic_drain_after_error(str, rc);
        if (rc != hipSuccess)
            return hip_pic_rc_to_errno(rc);
        at += p->rows * p->row_bytes;
    }
    return 0;
}

int vmaf_hip_picture_staging_alloc(void **out, size_t size)
{
    if (out == NULL || size == 0u)
        return -EINVAL;
    void *pinned = NULL;
    const hipError_t rc = hipHostMalloc(&pinned, size, hipHostMallocDefault);
    if (rc != hipSuccess)
        return hip_pic_rc_to_errno(rc);
    *out = pinned;
    return 0;
}

void vmaf_hip_picture_staging_free(void *staging)
{
    if (staging == NULL)
        return;
    /* Best-effort teardown: the owner drained the stream that read it. */
    const hipError_t rc = hipHostFree(staging);
    (void)rc;
}

int vmaf_hip_picture_alloc(VmafHipContext *ctx, void **out, size_t size)
{
    if (!ctx)
        return -EINVAL;
    if (!out)
        return -EINVAL;
    if (size == 0)
        return -EINVAL;

    void *device = NULL;
    hipError_t rc = hipMalloc(&device, size);
    if (rc != hipSuccess)
        return hip_pic_rc_to_errno(rc);

    /* hipMalloc succeeded: device pointer must be non-NULL. */
    assert(device != NULL);
    *out = device;
    return 0;
}

void vmaf_hip_picture_free(VmafHipContext *ctx, void *buf)
{
    (void)ctx;
    if (!buf)
        return;
    (void)hipFree(buf);
}

#else /* !HAVE_HIPCC — compile without the HIP runtime */
#include <errno.h>

#include "log.h"

int vmaf_hip_picture_alloc(VmafHipContext *ctx, void **out, size_t size)
{
    (void)ctx;
    (void)out;
    (void)size;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_hip_picture_alloc requires HIP support compiled with -Denable_hipcc=true\n");
    return -ENOSYS;
}

void vmaf_hip_picture_free(VmafHipContext *ctx, void *buf)
{
    (void)ctx;
    (void)buf;
}

int vmaf_hip_picture_upload(const VmafHipPlaneUpload *planes, unsigned n_planes, uintptr_t stream)
{
    (void)planes;
    (void)n_planes;
    (void)stream;
    vmaf_log(VMAF_LOG_LEVEL_ERROR,
             "vmaf_hip_picture_upload requires HIP support compiled with -Denable_hipcc=true\n");
    return -ENOSYS;
}

int vmaf_hip_picture_upload_staged(const VmafHipPlaneUpload *planes, unsigned n_planes,
                                   void *staging, size_t staging_bytes, uintptr_t stream)
{
    (void)planes;
    (void)n_planes;
    (void)staging;
    (void)staging_bytes;
    (void)stream;
    vmaf_log(VMAF_LOG_LEVEL_ERROR, "vmaf_hip_picture_upload_staged requires HIP support compiled "
                                   "with -Denable_hipcc=true\n");
    return -ENOSYS;
}

int vmaf_hip_picture_staging_alloc(void **out, size_t size)
{
    (void)out;
    (void)size;
    return -ENOSYS;
}

void vmaf_hip_picture_staging_free(void *staging)
{
    (void)staging;
}

#endif /* HAVE_HIPCC */
