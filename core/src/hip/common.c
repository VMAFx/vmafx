/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-3-Clause-Plus-Patent
 *
 *  HIP backend common surface — runtime PR (T7-10b / ADR-0212
 *  §"What lands next" steps 1+2).
 *
 *  Replaces the audit-first `-ENOSYS` stubs with real ROCm HIP
 *  runtime calls. Mirrors libvmaf/src/vulkan/common.c.
 *
 *    - `vmaf_hip_device_count`  -> hipGetDeviceCount
 *    - `vmaf_hip_state_init`    -> hipSetDevice +
 *                                  hipStreamCreateWithFlags
 *    - `vmaf_hip_state_free`    -> hipStreamDestroy + free
 *    - `vmaf_hip_list_devices`  -> hipGetDeviceCount +
 *                                  hipGetDeviceProperties (logs one
 *                                  line per device, returns count)
 *
 *  `vmaf_hip_import_state` lives in `libvmaf/src/libvmaf.c` (next to
 *  the CUDA / SYCL / Vulkan / Metal `_import_state` twins) because it
 *  needs `VmafContext` field-level access to stash the borrowed state
 *  pointer. Removed from this TU by ADR-0519.
 */

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>

#define __HIP_PLATFORM_AMD__ 1
#include <hip/hip_runtime_api.h>

#include "common.h"
#include "log.h"

#include "libvmaf/libvmaf_hip.h"

struct VmafHipContext {
    int device_index;
    /* hipStream_t handle stashed as uintptr_t for header purity (the
     * public `common.h` stays free of `<hip/hip_runtime.h>`). */
    uintptr_t stream;
};

/* Public-API wrapper. Same lifetime model as `VmafCudaState`: caller
 * owns the allocation, libvmaf borrows it for the duration of an
 * imported VmafContext. */
struct VmafHipState {
    struct VmafHipContext ctx;
};

int vmaf_hip_context_new(VmafHipContext **out, int device_index)
{
    if (out == NULL) {
        return -EINVAL;
    }
    VmafHipContext *ctx = calloc(1, sizeof(*ctx));
    if (ctx == NULL) {
        return -ENOMEM;
    }
    ctx->device_index = device_index;
    ctx->stream = 0;
    *out = ctx;
    return 0;
}

void vmaf_hip_context_destroy(VmafHipContext *ctx)
{
    if (ctx == NULL) {
        return;
    }
    if (ctx->stream != 0) {
        (void)hipStreamDestroy((hipStream_t)ctx->stream);
        ctx->stream = 0;
    }
    free(ctx);
}

int vmaf_hip_device_count(void)
{
    int n = 0;
    hipError_t rc = hipGetDeviceCount(&n);
    if (rc != hipSuccess) {
        /* No device or no runtime — return 0 (CUDA convention; the
         * public caller pivots on count > 0, not on the error code). */
        return 0;
    }
    return n;
}

/* ---- Public C-API entry points (libvmaf_hip.h) ---- */

int vmaf_hip_available(void)
{
#ifdef HAVE_HIP
    return 1;
#else
    return 0;
#endif
}

int vmaf_hip_state_init(VmafHipState **out, VmafHipConfiguration cfg)
{
    if (out == NULL) {
        return -EINVAL;
    }
    /* NASA P10 r5: pin the post-validation invariants the rest of
     * the function depends on. */
    assert(out != NULL);
    *out = NULL;

    int n = 0;
    hipError_t hip_rc = hipGetDeviceCount(&n);
    if (hip_rc != hipSuccess || n <= 0) {
        return -ENODEV;
    }
    assert(n > 0);

    int device_index = cfg.device_index;
    if (device_index < 0) {
        device_index = 0;
    }
    if (device_index >= n) {
        return -EINVAL;
    }
    assert(device_index >= 0 && device_index < n);

    hip_rc = hipSetDevice(device_index);
    if (hip_rc != hipSuccess) {
        return -ENODEV;
    }

    VmafHipState *s = calloc(1, sizeof(*s));
    if (s == NULL) {
        return -ENOMEM;
    }
    s->ctx.device_index = device_index;

    hipStream_t stream = NULL;
    hip_rc = hipStreamCreateWithFlags(&stream, hipStreamNonBlocking);
    if (hip_rc != hipSuccess) {
        free(s);
        return -EIO;
    }
    s->ctx.stream = (uintptr_t)stream;
    *out = s;
    return 0;
}

/* vmaf_hip_import_state moved to libvmaf/src/libvmaf.c — needs
 * VmafContext field-level access. ADR-0519. */

void vmaf_hip_state_free(VmafHipState **state)
{
    if (state == NULL || *state == NULL) {
        return;
    }
    VmafHipState *s = *state;
    if (s->ctx.stream != 0) {
        (void)hipStreamDestroy((hipStream_t)s->ctx.stream);
        s->ctx.stream = 0;
    }
    free(s);
    *state = NULL;
}

int vmaf_hip_list_devices(void)
{
    int n = 0;
    hipError_t rc = hipGetDeviceCount(&n);
    if (rc != hipSuccess) {
        return 0;
    }
    assert(n >= 0);
    for (int i = 0; i < n; ++i) {
        hipDeviceProp_t prop;
        rc = hipGetDeviceProperties(&prop, i);
        if (rc != hipSuccess) {
            continue;
        }
        vmaf_log(VMAF_LOG_LEVEL_INFO, "HIP device %d: %s (arch %s)\n", i, prop.name,
                 prop.gcnArchName);
    }
    assert(n >= 0);
    return n;
}
