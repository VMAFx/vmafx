/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Motion SAD pipeline shared by motion_cuda and motion_v2_cuda.
 *
 *  Both extractors report the CPU reference's motion SAD
 *  (integer_motion.c / integer_motion_v2.c, motion_score_pipeline_8/_16):
 *
 *    diff = prev - cur                           raw samples, reflect-101 borders
 *    v    = (sum_k filter[k] * diff[y+k-2] + 2^(bpc-1)) >> bpc
 *    h    = (sum_k filter[k] * v[x+k-2] + 2^15) >> 16
 *    SAD  = sum |h|
 *
 *  The frames are differenced before the blur and each pass rounds, exactly
 *  like the CPU. Blurring each frame and differencing the blurred frames is
 *  the same sum only without rounding; motion_cuda did it that way and drifted
 *  from the CPU (T-CUDA-MOTION-BLUR-THEN-DIFF-2026-09-29, ADR-1372; the SYCL
 *  twin measured 2e-4 at 17x17 and 1.3e-5 on the Netflix 576x324 pair before
 *  ADR-1371 fixed it the same way).
 *
 *  The one kernel lives in integer_motion_v2/motion_v2_score.cu; this helper
 *  is the only host code that loads and launches it. Every step of a frame is
 *  enqueued on the picture's stream and ordered against the previous frame by
 *  an event, never by a host wait.
 */

#ifndef VMAF_FEATURE_CUDA_INTEGER_MOTION_SAD_CUDA_H_
#define VMAF_FEATURE_CUDA_INTEGER_MOTION_SAD_CUDA_H_

#include <stddef.h>

#include "common.h"
#include "cuda_helper.cuh"
#include "libvmaf/picture.h"

/* The loaded SAD module and its two kernels. Zero-initialised state is the
 * "not loaded" state; unload is safe on it. */
typedef struct MotionSadCuda {
    CUmodule module;
    CUfunction sad_8bpc;
    CUfunction sad_16bpc;
} MotionSadCuda;

/* One frame of a raw-luma ping-pong. Both planes are packed `width` x
 * `height` samples of `bpc <= 8 ? 1 : 2` bytes. */
typedef struct MotionSadFrame {
    VmafPicture *pic;  /* this frame's reference picture, on the device */
    CUdeviceptr cur;   /* receives pic's luma, the next frame's `prev` */
    CUdeviceptr prev;  /* the previous frame's luma; 0 on the first frame */
    CUdeviceptr sad;   /* uint64 accumulator, zeroed here; unused when prev is 0 */
    CUevent prev_done; /* recorded after the previous frame's work; NULL on the first */
    unsigned width;
    unsigned height;
    unsigned bpc;
} MotionSadFrame;

/* Bytes of one packed luma plane. */
size_t vmaf_cuda_motion_sad_plane_bytes(unsigned width, unsigned height, unsigned bpc);

/* Load the module and resolve both kernels in `cu_state`'s context. On
 * failure the caller's unload path releases whatever was loaded. */
int vmaf_cuda_motion_sad_load(VmafCudaState *cu_state, MotionSadCuda *sad);

/* Unload the module with its owning context current (ADR-1336). */
int vmaf_cuda_motion_sad_unload(VmafCudaState *cu_state, MotionSadCuda *sad);

/* Enqueue one frame on `stream`: wait for the picture's upload and for
 * `prev_done`, copy the picture's luma into `cur`, and, when `prev` is set,
 * zero `sad` and add sum(|blur(prev - cur)|) into it. The wait on
 * `prev_done` orders the copy after the previous frame's kernel read the
 * slot it overwrites, and the kernel after the previous frame's copy filled
 * `prev`; both run on the device. Integer arithmetic throughout, so the SAD
 * is the CPU's for every order of the atomic adds. */
int vmaf_cuda_motion_sad_submit(const MotionSadCuda *sad, CudaFunctions *cu_f, CUstream stream,
                                const MotionSadFrame *frame);

#endif /* VMAF_FEATURE_CUDA_INTEGER_MOTION_SAD_CUDA_H_ */
