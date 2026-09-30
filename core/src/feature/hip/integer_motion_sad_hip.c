/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Motion SAD pipeline shared by motion_hip and motion_v2_hip; see
 *  integer_motion_sad_hip.h for the contract (ADR-1377).
 */

#include "integer_motion_sad_hip.h"

#include <stddef.h>

size_t vmaf_hip_motion_sad_plane_bytes(unsigned width, unsigned height, unsigned bpc)
{
    return (size_t)width * height * ((bpc <= 8u) ? 1u : 2u);
}

#ifdef HAVE_HIPCC

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <hip/hip_runtime_api.h>

#include "../../hip/common.h"
#include "../../hip/hip_handle.h"
#include "../../hip/picture_hip.h"
#include "integer_motion_v2_hip.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Block shape of the kernels: MV2_BLOCK_X x MV2_BLOCK_Y in motion_v2_score.hip. */
#define MOTION_SAD_HIP_BX 16u
#define MOTION_SAD_HIP_BY 16u

int vmaf_hip_motion_sad_load(VmafHipMotionSad *k)
{
    if (k == NULL)
        return -EINVAL;
    hipError_t rc = hipModuleLoadData(&k->module, motion_v2_score_hsaco);
    if (rc != hipSuccess) {
        k->module = NULL;
        return vmaf_hip_rc_to_errno(rc);
    }
    rc = hipModuleGetFunction(&k->func_8bpc, k->module, "motion_v2_kernel_8bpc");
    if (rc == hipSuccess)
        rc = hipModuleGetFunction(&k->func_16bpc, k->module, "motion_v2_kernel_16bpc");
    if (rc != hipSuccess) {
        vmaf_hip_motion_sad_unload(k);
        return vmaf_hip_rc_to_errno(rc);
    }
    return 0;
}

void vmaf_hip_motion_sad_unload(VmafHipMotionSad *k)
{
    if (k == NULL || k->module == NULL)
        return;
    /* Best-effort teardown: the handle is dropped whatever the runtime says. */
    const hipError_t rc = hipModuleUnload(k->module);
    (void)rc;
    k->module = NULL;
    k->func_8bpc = NULL;
    k->func_16bpc = NULL;
}

/* Zero the accumulator, then add sum |blur(prev - cur)| into it. */
static int motion_sad_launch(const VmafHipMotionSad *k, const VmafHipMotionSadFrame *f,
                             hipStream_t str)
{
    hipError_t rc = hipMemsetAsync(f->sad, 0, sizeof(uint64_t), str);
    if (rc != hipSuccess)
        return vmaf_hip_rc_to_errno(rc);

    const void *prev = f->prev;
    const void *cur = f->cur;
    ptrdiff_t pitch = (ptrdiff_t)(vmaf_hip_motion_sad_plane_bytes(f->width, 1u, f->bpc));
    uint64_t *sad = f->sad;
    unsigned w = f->width;
    unsigned h = f->height;
    unsigned bpc = f->bpc;
    /* The 16bpc kernel takes one more argument than the 8bpc one: `bpc`. */
    void *args8[] = {(void *)&prev, (void *)&cur, (void *)&pitch, (void *)&pitch,
                     (void *)&sad,  (void *)&w,   (void *)&h};
    void *args16[] = {(void *)&prev, (void *)&cur, (void *)&pitch, (void *)&pitch,
                      (void *)&sad,  (void *)&w,   (void *)&h,     (void *)&bpc};
    const bool is8 = (bpc <= 8u);
    const unsigned gx = (w + MOTION_SAD_HIP_BX - 1u) / MOTION_SAD_HIP_BX;
    const unsigned gy = (h + MOTION_SAD_HIP_BY - 1u) / MOTION_SAD_HIP_BY;
    rc = hipModuleLaunchKernel(is8 ? k->func_8bpc : k->func_16bpc, gx, gy, 1, MOTION_SAD_HIP_BX,
                               MOTION_SAD_HIP_BY, 1, 0, str, is8 ? args8 : args16, NULL);
    return vmaf_hip_rc_to_errno(rc);
}

/* Error path only: the upload is already enqueued from `staging` into `cur`,
 * so wait for it before the caller may reuse either, then pass `err` on. */
static int motion_sad_drain_after_error(uintptr_t stream, int err)
{
    const hipError_t rc = hipStreamSynchronize(vmaf_hip_stream_of(stream));
    (void)rc; /* `err` is the failure to report */
    return err;
}

int vmaf_hip_motion_sad_submit(const VmafHipMotionSad *k, const VmafHipMotionSadFrame *frame,
                               uintptr_t stream)
{
    if (k == NULL || frame == NULL || k->module == NULL || frame->pic == NULL ||
        frame->cur == NULL || frame->staging == NULL)
        return -EINVAL;
    const size_t row_bytes = vmaf_hip_motion_sad_plane_bytes(frame->width, 1u, frame->bpc);
    const VmafHipPlaneUpload plane = {.dst = frame->cur,
                                      .dst_pitch = row_bytes,
                                      .pic = frame->pic,
                                      .plane = 0u,
                                      .row_bytes = row_bytes,
                                      .rows = frame->height};
    /* The host copy reads the picture now; the device copy runs later from
     * `staging`, ahead of the kernel on the same stream. No host wait. */
    const int err =
        vmaf_hip_picture_upload_staged(&plane, 1u, frame->staging, frame->staging_bytes, stream);
    if (err != 0 || frame->prev == NULL)
        return err;
    const int launch_err = motion_sad_launch(k, frame, vmaf_hip_stream_of(stream));
    if (launch_err != 0)
        return motion_sad_drain_after_error(stream, launch_err);
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* HAVE_HIPCC */
