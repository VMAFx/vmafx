/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
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
 */

#include <stddef.h>

#include "picture_hip.h"

/* Pull the HIP runtime only when the toolchain provides it.  Non-HIP
 * translation units include this file through `hip/meson.build`'s
 * `hip_sources` list, which is only activated when HAVE_HIPCC is set. */
#ifdef HAVE_HIPCC
#include <assert.h>
#include <errno.h>
#include <hip/hip_runtime_api.h>

/* Translate a HIP error to a negative POSIX errno.  Mirrors the static
 * helper present in every feature/hip/ extractor; cannot use the shared
 * vmaf_hip_rc_to_errno declared in common.h because picture_hip.c is
 * compiled as plain C and common.c carries the hipcc path separately. */
static int hip_pic_rc_to_errno(hipError_t rc)
{
    switch (rc) {
    case hipSuccess:
        return 0;
    case hipErrorInvalidValue:
        return -EINVAL;
    case hipErrorOutOfMemory:
        return -ENOMEM;
    case hipErrorNotInitialized:
        return -EIO;
    case hipErrorDeinitialized:
        return -EIO;
    case hipErrorNoDevice:
        return -ENODEV;
    default:
        return -EIO;
    }
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

int vmaf_hip_picture_alloc(VmafHipContext *ctx, void **out, size_t size)
{
    (void)ctx;
    (void)out;
    (void)size;
    return -ENOSYS;
}

void vmaf_hip_picture_free(VmafHipContext *ctx, void *buf)
{
    (void)ctx;
    (void)buf;
}

#endif /* HAVE_HIPCC */
