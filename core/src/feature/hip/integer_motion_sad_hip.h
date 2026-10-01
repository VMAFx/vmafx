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
 *  is the only host code that loads and launches it. A frame reads the
 *  reference luma the extractor acquired on the device (the context's shared
 *  frame, ADR-1408, or the extractor's own copy): from the second frame on,
 *  the SAD against the luma kept from the previous frame, then a
 *  device-to-device copy that keeps this frame's luma for the next one, all
 *  enqueued on the extractor's private stream. Nothing here waits on the
 *  device.
 */

#ifndef FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_
#define FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_

#include <stdbool.h>
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

/* One frame. `cur` and `keep` are packed device planes of `width` x `height`
 * samples, vmaf_hip_motion_sad_plane_bytes() each. `cur` is this frame's
 * luma and is only read; `keep` is the extractor's own plane, holding the
 * previous frame's luma when `have_prev` is set and receiving this frame's
 * after the SAD. */
typedef struct VmafHipMotionSadFrame {
    const void *cur; /* this frame's reference luma on the device */
    void *keep;      /* the previous frame's luma; becomes this frame's */
    bool have_prev;  /* false on the first frame: no SAD, only the copy */
    uint64_t *sad;   /* device accumulator, zeroed here; unused without prev */
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
 * kernel-template convention): when `have_prev` is set, zero `sad` and add
 * sum |blur(keep - cur)| into it; then copy `cur` into `keep`, device to
 * device, behind the SAD on the same stream. `cur` must stay untouched until
 * the stream has run both, which the extractor's collect() waits for.
 * Returns 0 or a negative errno; when the copy fails to enqueue after the
 * SAD was, it waits for the stream first, so no kernel is still reading
 * `keep` or `cur` once the error is returned (the one host wait, error path
 * only). */
int vmaf_hip_motion_sad_submit(const VmafHipMotionSad *k, const VmafHipMotionSadFrame *frame,
                               uintptr_t stream);

#endif /* HAVE_HIPCC */

#endif /* FEATURE_HIP_INTEGER_MOTION_SAD_HIP_H_ */
