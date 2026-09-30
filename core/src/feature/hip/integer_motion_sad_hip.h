/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Motion SAD pipeline shared by motion_hip and motion_v2_hip (ADR-1377).
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
 *  the same sum only without rounding; motion_hip did it that way and drifted
 *  from the CPU (T-HIP-MOTION-BLUR-THEN-DIFF-2026-09-29; the SYCL twin
 *  measured 2e-4 at 17x17 and 1.3e-5 on the Netflix 576x324 pair before
 *  ADR-1371 fixed it the same way).
 *
 *  The one kernel lives in integer_motion_v2/motion_v2_score.hip; this helper
 *  is the only host code that loads and launches it. A frame is one staged
 *  upload of the reference luma into a raw ping-pong slot and, from the
 *  second frame on, the SAD against the other slot, all enqueued on the
 *  extractor's private stream. Nothing here waits on the device: the
 *  extractor's collect() is the one wait of the frame.
 */

#ifndef FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_
#define FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_

#include <stddef.h>
#include <stdint.h>

/* Bytes of one packed luma plane. Host arithmetic only, so it exists in the
 * scaffold build (no enable_hipcc) as well. */
size_t vmaf_hip_motion_sad_plane_bytes(unsigned width, unsigned height, unsigned bpc);

#ifdef HAVE_HIPCC

#include <hip/hip_runtime_api.h>

#include "libvmaf/picture.h"

/* The embedded HSACO module of motion_v2_score.hip and its two entry points:
 * uint8 samples (int32 vertical sum) and uint16 samples (int64 vertical sum,
 * every bit depth above 8). Zero-initialised state is "not loaded". */
typedef struct VmafHipMotionSad {
    hipModule_t module;
    hipFunction_t func_8bpc;
    hipFunction_t func_16bpc;
} VmafHipMotionSad;

/* One frame of a raw-luma ping-pong. `cur` and `prev` are packed device
 * planes of `width` x `height` samples, vmaf_hip_motion_sad_plane_bytes()
 * each; `staging` is pinned host memory (vmaf_hip_picture_staging_alloc())
 * of `staging_bytes`, the size the owner allocated, so the upload refuses a
 * plane that does not fit instead of trusting the frame geometry. */
typedef struct VmafHipMotionSadFrame {
    const VmafPicture *pic; /* this frame's reference picture, in host memory */
    void *staging;          /* host copy of pic's luma, the device copy's source */
    size_t staging_bytes;   /* allocated size of `staging` */
    void *cur;              /* receives pic's luma: the next frame's `prev` */
    const void *prev;       /* the previous frame's luma; NULL on the first frame */
    uint64_t *sad;          /* device accumulator, zeroed here; unused without prev */
    unsigned width;
    unsigned height;
    unsigned bpc;
} VmafHipMotionSadFrame;

/* Load the module and resolve both kernels. On failure nothing stays
 * loaded. Returns 0 or a negative errno. */
int vmaf_hip_motion_sad_load(VmafHipMotionSad *k);

/* Unload the module. Safe on a zeroed or already unloaded handle. */
void vmaf_hip_motion_sad_unload(VmafHipMotionSad *k);

/* Enqueue one frame on `stream` (a hipStream_t carried as uintptr_t, the
 * kernel-template convention): stage pic's luma into `cur` through
 * `staging` without waiting (vmaf_hip_picture_upload_staged()) and, when
 * `prev` is set, zero `sad` and add sum |blur(prev - cur)| into it. The
 * picture is read before this returns. Returns 0 or a negative errno; when
 * the SAD fails to enqueue after the upload was, it waits for the stream
 * first, so no copy is still reading `staging` or writing `cur` once the
 * error is returned (the one host wait, error path only). */
int vmaf_hip_motion_sad_submit(const VmafHipMotionSad *k, const VmafHipMotionSadFrame *frame,
                               uintptr_t stream);

#endif /* HAVE_HIPCC */

#endif /* FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_ */
