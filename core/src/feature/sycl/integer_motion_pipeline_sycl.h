/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Motion SAD pipeline shared by motion_sycl and motion_v2_sycl.
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
 *  the same sum only without rounding: the two per-frame roundings leave up
 *  to one unit per pixel, and a SYCL twin that did it that way drifted from
 *  the CPU by 2e-4 on 17x17 frames and 1.3e-5 on the Netflix 576x324 pair
 *  (T-SYCL-MOTION-TINY-FRAME-PARITY-2026-09-29, ADR-1371).
 *
 *  The kernel lives in integer_motion_pipeline_sycl.cpp only: anonymous kernel
 *  lambdas in two translation units can receive the same generated name
 *  (Research-2090), so neither extractor TU defines one.
 */

#ifndef VMAF_FEATURE_SYCL_INTEGER_MOTION_PIPELINE_SYCL_H_
#define VMAF_FEATURE_SYCL_INTEGER_MOTION_PIPELINE_SYCL_H_

#include <sycl/sycl.hpp>

#include <cstdint>

namespace motion_sycl_pipeline
{

/* One plane pair, packed `width` x `height` samples (uint8 for bpc <= 8,
 * uint16 otherwise). */
struct SadArgs {
    const void *prev; /* previous frame; unused by enqueue_copy() */
    const void *cur;  /* current frame */
    void *cur_copy;   /* receives `cur` for the next frame; nullptr when the caller keeps it */
    int64_t *sad;     /* device accumulator, zeroed by the caller */
    unsigned width;
    unsigned height;
    unsigned bpc;
};

/* Enqueue sum(|blur(prev - cur)|) into *args.sad and, when args.cur_copy is
 * set, the copy of `cur` into it. `queue` must be in-order. */
void enqueue_sad(sycl::queue &queue, const SadArgs &args);

/* Enqueue only the copy of `cur` into `cur_copy`: the first frame has no
 * previous frame to difference against. */
void enqueue_copy(sycl::queue &queue, const SadArgs &args);

} // namespace motion_sycl_pipeline

#endif /* VMAF_FEATURE_SYCL_INTEGER_MOTION_PIPELINE_SYCL_H_ */
