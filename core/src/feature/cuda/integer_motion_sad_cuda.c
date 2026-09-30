/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Host side of the motion SAD pipeline shared by motion_cuda and
 *  motion_v2_cuda; see integer_motion_sad_cuda.h for the arithmetic
 *  contract. One module, one pair of kernels, one launch geometry.
 */

#include <errno.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "picture_cuda.h"
#include "cuda/integer_motion_sad_cuda.h"
#include "cuda/integer_motion_v2_cuda.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

/* Must match MV2_BLOCK_X / MV2_BLOCK_Y in motion_v2_score.cu: the kernel's
 * shared tile and __launch_bounds__ assume a 16x16 block. */
#define MOTION_SAD_BLOCK_X 16u
#define MOTION_SAD_BLOCK_Y 16u

size_t vmaf_cuda_motion_sad_plane_bytes(unsigned width, unsigned height, unsigned bpc)
{
    return (size_t)width * height * ((bpc <= 8u) ? 1u : 2u);
}

/* Load and resolve with the owning context already current. */
static int motion_sad_load_current(CudaFunctions *cu_f, MotionSadCuda *sad)
{
    CHECK_CUDA_RETURN(cu_f, cuModuleLoadData(&sad->module, motion_v2_score_ptx));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&sad->sad_8bpc, sad->module, "motion_v2_kernel_8bpc"));
    CHECK_CUDA_RETURN(cu_f,
                      cuModuleGetFunction(&sad->sad_16bpc, sad->module, "motion_v2_kernel_16bpc"));
    return 0;
}

int vmaf_cuda_motion_sad_load(VmafCudaState *cu_state, MotionSadCuda *sad)
{
    if (!cu_state || !sad)
        return -EINVAL;
    CudaFunctions *cu_f = cu_state->f;
    CHECK_CUDA_RETURN(cu_f, cuCtxPushCurrent(cu_state->ctx));
    const int err = motion_sad_load_current(cu_f, sad);
    /* Pop on every path; the load's own error is the one to report. */
    const CUresult pop_res = cu_f->cuCtxPopCurrent(NULL);
    if (err)
        return err;
    return vmaf_cuda_result_to_errno((int)pop_res);
}

int vmaf_cuda_motion_sad_unload(VmafCudaState *cu_state, MotionSadCuda *sad)
{
    if (!sad)
        return 0;
    const int err = vmaf_cuda_module_unload(cu_state, &sad->module);
    if (!err) {
        sad->sad_8bpc = NULL;
        sad->sad_16bpc = NULL;
    }
    return err;
}

/* Packed copy of the picture's luma into frame->cur: the plane may be
 * pitched, the copy is not, so the kernel reads prev and cur with one pitch. */
static int motion_sad_stage(CudaFunctions *cu_f, CUstream stream, const MotionSadFrame *frame)
{
    const size_t row_bytes = (size_t)frame->width * ((frame->bpc <= 8u) ? 1u : 2u);
    /* Designated fields only: both memory types are set, every other field
     * is the zero the driver expects. */
    const CUDA_MEMCPY2D copy = {
        .srcMemoryType = CU_MEMORYTYPE_DEVICE,
        .srcDevice = (CUdeviceptr)frame->pic->data[0],
        .srcPitch = frame->pic->stride[0],
        .dstMemoryType = CU_MEMORYTYPE_DEVICE,
        .dstDevice = frame->cur,
        .dstPitch = row_bytes,
        .WidthInBytes = row_bytes,
        .Height = frame->height,
    };
    CHECK_CUDA_RETURN(cu_f, cuMemcpy2DAsync(&copy, stream));
    return 0;
}

/* Zero the accumulator and launch the kernel. The memset shares the kernel's
 * stream: the two are ordered only by that (ADR-0358). */
static int motion_sad_launch(const MotionSadCuda *sad, CudaFunctions *cu_f, CUstream stream,
                             const MotionSadFrame *frame)
{
    CHECK_CUDA_RETURN(cu_f, cuMemsetD8Async(frame->sad, 0, sizeof(uint64_t), stream));

    ptrdiff_t pitch = (ptrdiff_t)frame->width * ((frame->bpc <= 8u) ? 1 : 2);
    CUdeviceptr prev = frame->prev;
    CUdeviceptr cur = frame->cur;
    CUdeviceptr acc = frame->sad;
    unsigned width = frame->width;
    unsigned height = frame->height;
    unsigned bpc = frame->bpc;
    const unsigned grid_x = DIV_ROUND_UP(width, MOTION_SAD_BLOCK_X);
    const unsigned grid_y = DIV_ROUND_UP(height, MOTION_SAD_BLOCK_Y);

    /* Parameter order matches motion_v2_kernel_{8,16}bpc exactly; both take
     * the same eight parameters (ADR-1215: cuLaunchKernel checks neither
     * count nor order). */
    void *params[] = {&prev, &cur, &pitch, &pitch, &acc, &width, &height, &bpc};
    CUfunction kernel = (bpc <= 8u) ? sad->sad_8bpc : sad->sad_16bpc;
    CHECK_CUDA_RETURN(cu_f, cuLaunchKernel(kernel, grid_x, grid_y, 1, MOTION_SAD_BLOCK_X,
                                           MOTION_SAD_BLOCK_Y, 1, 0, stream, params, NULL));
    return 0;
}

int vmaf_cuda_motion_sad_submit(const MotionSadCuda *sad, CudaFunctions *cu_f, CUstream stream,
                                const MotionSadFrame *frame)
{
    if (!sad || !cu_f || !frame || !frame->pic)
        return -EINVAL;
    CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, vmaf_cuda_picture_get_ready_event(frame->pic),
                                              CU_EVENT_WAIT_DEFAULT));
    if (frame->prev_done)
        CHECK_CUDA_RETURN(cu_f, cuStreamWaitEvent(stream, frame->prev_done, CU_EVENT_WAIT_DEFAULT));

    const int stage_err = motion_sad_stage(cu_f, stream, frame);
    if (stage_err || !frame->prev)
        return stage_err;
    return motion_sad_launch(sad, cu_f, stream, frame);
}

/* NOLINTEND(modernize-use-nullptr) */
