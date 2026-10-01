/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  The sum of absolute differences of float_motion_hip, in the CPU's order
 *  (ADR-1409).
 *
 *  float_motion.c adds the absolute differences of one row, left to right,
 *  into one `float` (float_sad_line(), and the inner loop of
 *  motion.c::vmaf_image_sad_c()), adds the row sums top to bottom into
 *  another, and divides by the pixel count in `float`. Every step of a
 *  running fp32 sum rounds, so the score depends on that order: a twin that
 *  reduces per block, as this one did, is 3e-6 from the CPU at 576x324 and up
 *  to 1.4e-4 at 1920x1080. So the device adds each row in the CPU's order,
 *  one thread per row, and returns one fp32 sum per row; the host adds the
 *  rows (feature/float_motion_sad.h).
 *
 *  A row sum is one dependency chain of `width` additions, so a thread can
 *  only walk its row sample by sample, and the threads of a wave walk
 *  different rows. Read from the blurred planes, every step of a wave would
 *  touch one cache line per row (a gfx1036 then needs 145 ms for a 3840x2160
 *  frame, where the whole twin took 18). The absolute differences are
 *  therefore computed first, one thread per sample, and stored transposed:
 *  the rows are grouped in sets of VMAF_HIP_FLOAT_MOTION_ROW_GROUP, and
 *  within a group the samples of one column are adjacent. The threads that
 *  add a group's rows, one block of the row kernel, then read consecutive
 *  memory at every step.
 *
 *  `motion_add_scale1` adds a second term: the same sum over both blurred
 *  frames scaled to half size with motion.c::motion_scale_bilinear(). Its
 *  differences are stored the same way and added by the same row kernel.
 *
 *  Plain C as well as HIP C++: float_motion_score.hip and the device-free
 *  test_hip_float_motion_rows.c compile the same lines, and the test holds
 *  them against motion.c::compute_motion() itself. The kernels build with
 *  `hip_strict_fp_args` (ADR-1407), the test with contraction off: none of
 *  the products and sums here may be fused.
 */

#ifndef VMAF_SRC_FEATURE_HIP_FLOAT_MOTION_FLOAT_MOTION_ROWS_H_
#define VMAF_SRC_FEATURE_HIP_FLOAT_MOTION_FLOAT_MOTION_ROWS_H_

#include <math.h>
#include <stddef.h>

#include "../hip_tile_index.h"

/* Rows per group of a transposed difference plane, and the threads per block
 * of the row kernel. A multiple of both wave sizes (32 and 64) and of the
 * blur kernel's block height. */
#define VMAF_HIP_FLOAT_MOTION_ROW_GROUP 64u

/* Floats in the transposed difference plane of a `width` x `height` plane:
 * whole groups of rows. The rows past `height` in the last group are never
 * written or read. */
VMAF_HIP_HOST_DEVICE size_t vmaf_hip_float_motion_diff_count(unsigned width, unsigned height)
{
    const size_t groups =
        ((size_t)height + VMAF_HIP_FLOAT_MOTION_ROW_GROUP - 1u) / VMAF_HIP_FLOAT_MOTION_ROW_GROUP;
    return groups * width * VMAF_HIP_FLOAT_MOTION_ROW_GROUP;
}

/* Where sample (x, y) sits in a transposed difference plane of `width`
 * columns: group, then column, then the row within the group. */
VMAF_HIP_HOST_DEVICE size_t vmaf_hip_float_motion_diff_index(unsigned x, unsigned y, unsigned width)
{
    const size_t group = y / VMAF_HIP_FLOAT_MOTION_ROW_GROUP;
    const size_t lane = y % VMAF_HIP_FLOAT_MOTION_ROW_GROUP;
    return ((group * width) + x) * VMAF_HIP_FLOAT_MOTION_ROW_GROUP + lane;
}

/* One term of float_sad_line(): `diff < 0 ? -diff : diff` of two samples. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_float_motion_abs_diff(float cur, float prev)
{
    const float diff = cur - prev;
    return (diff < 0.0f) ? -diff : diff;
}

/* float_sad_line() of row `y`: its `width` absolute differences, read from
 * the transposed plane `diff` and added left to right into one fp32
 * accumulator. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_float_motion_row_sum(const float *diff, unsigned width,
                                                         unsigned y)
{
    const float *row = diff + vmaf_hip_float_motion_diff_index(0u, y, width);
    float accum = 0.0f;
    for (unsigned j = 0u; j < width; j++) {
        accum += row[(size_t)j * VMAF_HIP_FLOAT_MOTION_ROW_GROUP];
    }
    return accum;
}

/* motion.c::motion_mirror_f() for a left edge of 0. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_float_motion_reflect(float i, float right)
{
    if (i < 0.0f) {
        return -i;
    }
    return (i > right) ? (2.0f * right) - i : i;
}

/* motion.c::motion_bilinear_interp(): the sample of a packed `width` x
 * `height` plane at (x, y), every operand in the CPU's order. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_float_motion_bilinear(const float *src, unsigned width,
                                                          unsigned height, float x, float y)
{
    const float right = (float)(width - 1u);
    const float bottom = (float)(height - 1u);
    const int x1 = (int)vmaf_hip_float_motion_reflect(floorf(x), right);
    const int x2 = (int)vmaf_hip_float_motion_reflect(ceilf(x), right);
    const int y1 = (int)vmaf_hip_float_motion_reflect(floorf(y), bottom);
    const int y2 = (int)vmaf_hip_float_motion_reflect(ceilf(y), bottom);
    const float dx = x - (float)x1;
    const float dy = y - (float)y1;
    const size_t row1 = (size_t)y1 * width;
    const size_t row2 = (size_t)y2 * width;
    return ((1.0f - dy) * (1.0f - dx) * src[row1 + (size_t)x1] +
            (1.0f - dy) * dx * src[row1 + (size_t)x2] + dy * (1.0f - dx) * src[row2 + (size_t)x1] +
            dy * dx * src[row2 + (size_t)x2]);
}

/* The two blurred frames of one plane and the half-size plane
 * motion.c::vmaf_image_sad_c() scales them to. */
typedef struct VmafHipFloatMotionScale1 {
    const float *cur;
    const float *prev;
    unsigned width;
    unsigned height;
    unsigned scaled_width;
    unsigned scaled_height;
} VmafHipFloatMotionScale1;

/* Sample (x, y) of the scale-1 term: motion_scale_bilinear() of both frames
 * at that sample, and their absolute difference. */
VMAF_HIP_HOST_DEVICE float vmaf_hip_float_motion_scale1_abs_diff(const VmafHipFloatMotionScale1 *s,
                                                                 unsigned x, unsigned y)
{
    const float ratio_x = (float)s->width / (float)s->scaled_width;
    const float ratio_y = (float)s->height / (float)s->scaled_height;
    const float xx = ((float)x + 0.5f) * ratio_x - 0.5f;
    const float yy = ((float)y + 0.5f) * ratio_y - 0.5f;
    return vmaf_hip_float_motion_abs_diff(
        vmaf_hip_float_motion_bilinear(s->cur, s->width, s->height, xx, yy),
        vmaf_hip_float_motion_bilinear(s->prev, s->width, s->height, xx, yy));
}

#if !defined(__HIPCC__)

#include "feature/float_motion_sad.h"

/* The score of one plane from its row sums, as motion.c::vmaf_image_sad_c()
 * returns it: the scale-0 mean, plus the scale-1 mean in fp32 when
 * `scale1_rows` is not NULL (motion_add_scale1). Without the option this is
 * float_motion.c::compute_motion_simd(). */
static inline double vmaf_hip_float_motion_plane_score(const float *rows, unsigned width,
                                                       unsigned height, const float *scale1_rows,
                                                       unsigned scaled_width,
                                                       unsigned scaled_height)
{
    float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);
    if (scale1_rows != NULL) {
        score +=
            (float)vmaf_float_motion_score_from_row_sads(scale1_rows, scaled_width, scaled_height);
    }
    return (double)score;
}

#endif /* !defined(__HIPCC__) */

#endif /* VMAF_SRC_FEATURE_HIP_FLOAT_MOTION_FLOAT_MOTION_ROWS_H_ */
