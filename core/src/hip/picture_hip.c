/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2
 *
 *  Picture allocation / lifecycle for the HIP backend (ADR-0212 / T7-10).
 *
 *  Implements a single-allocation device-memory pool per call.  The API
 *  deliberately mirrors `vmaf_cuda_picture_alloc` / `vmaf_cuda_picture_free`
 *  from `libvmaf/src/cuda/picture_cuda.c` so the two backends share the same
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
 *  ADR-0613: fix P1-2 from the scaffold audit (libvmaf/src/hip/picture_hip.c
 *  previously returned -ENOSYS, blocking zero-copy upload for all HIP
 *  extractors).
 *
 *  vmaf_hip_picture_upload() is how every HIP extractor stages a host
 *  VmafPicture onto the device: it returns only once the copies have
 *  finished reading the picture (T-HIP-PAGEABLE-UPLOAD-RACE-2026-09-18).
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
    /* Wait even when a copy failed: the ones already enqueued still read the
     * pictures, and the caller may recycle them as soon as this returns. */
    rc = (enqueued > 0u) ? hip_pic_wait(done, str) : hipSuccess;
    (void)hipEventDestroy(done);
    if (copy_rc != hipSuccess)
        return hip_pic_rc_to_errno(copy_rc);
    return hip_pic_rc_to_errno(rc);
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

#endif /* HAVE_HIPCC */
