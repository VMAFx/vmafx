/**
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: EUPL-1.2 AND BSD-2-Clause-Patent
 *
 *  The arithmetic of float_motion_metal (ADR-1498), in the CPU's order.
 *
 *  float_motion.c converts each sample as picture_copy() does, blurs the
 *  plane with convolution_f32_c_s() (one 5-tap pass down the columns, then
 *  one along the rows, each tap one rounded fp32 product and one rounded
 *  fp32 sum, the edges mirrored reflect-101), and adds the absolute
 *  differences of two blurred frames row by row: left to right into one fp32
 *  accumulator per row, the rows top to bottom into another, divided in fp32
 *  (float_sad_line(), motion.c::vmaf_image_sad_c()). Every step rounds, so the
 *  score depends on that order; a per-block sum is up to 1.4e-4 off at
 *  1920x1080 (ADR-1409). `motion_add_scale1` adds the same mean over both
 *  blurred frames scaled to half size with motion.c::motion_scale_bilinear().
 *
 *  The kernels of float_motion.metal compute every value through this header,
 *  and core/test/test_metal_float_motion_math.cpp compiles the same lines on
 *  the host and holds them against the CPU's own functions, value by value.
 *  The design is float_motion_hip's (ADR-1404, ADR-1419): the blur stores
 *  |cur - prev| of every sample transposed in groups of
 *  VMAF_MTL_FM_ROW_GROUP rows, so that the threads of a SIMD group, one per
 *  row, read consecutive memory at every step of their row sums.
 *
 *  Written on metal_portable.h: values in and out, no `double` outside the
 *  host-only block at the end. No operation here is fused: the CPU fuses
 *  none of them, and the kernels build with -ffp-contract=off. Each product
 *  is named before it is added so that the rounding stays in the source.
 */

#ifndef VMAF_FEATURE_METAL_METAL_FLOAT_MOTION_MATH_H_
#define VMAF_FEATURE_METAL_METAL_FLOAT_MOTION_MATH_H_

#include "metal_portable.h"

/* cppcheck passedByValue: every function here takes its argument structs by
 * value because the header is shared with C and Metal Shading Language, which
 * have no const references (an MSL reference needs an address space), ADR-1498. */
// cppcheck-suppress-begin passedByValue

#if defined(__METAL_VERSION__)
/* The address space of a pointer to the thread's own window: an MSL pointer needs one. */
#define VMAF_MTL_FM_THR thread
#define VMAF_MTL_FM_FLOOR(x) metal::floor(x)
#define VMAF_MTL_FM_CEIL(x) metal::ceil(x)
#else
#define VMAF_MTL_FM_THR
#define VMAF_MTL_FM_FLOOR(x) floorf(x)
#define VMAF_MTL_FM_CEIL(x) ceilf(x)
#endif

/* Threadgroup edge of the blur and scale-1 kernels, the blur's radius and
 * taps, and the edge of the blur's threadgroup tile. */
#define VMAF_MTL_FM_BLOCK 16u
#define VMAF_MTL_FM_RADIUS 2
#define VMAF_MTL_FM_TAPS 5
#define VMAF_MTL_FM_TILE 20u

/* Rows per group of a transposed difference plane, and the threads per
 * threadgroup of the row kernel: two SIMD groups of 32. */
#define VMAF_MTL_FM_ROW_GROUP 64u

/* Arguments of the blur kernel. `filter_size` is the motion_filter_size
 * option, `compute_sad` 0 on the first frame. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmBlurArgs {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u32 bpc;
    vmaf_mtl_u32 filter_size;
    vmaf_mtl_u32 compute_sad;
} VmafMtlFmBlurArgs;

/* Arguments of the scale-1 kernel: the blurred plane, the half-size plane
 * and vmaf_mtl_fm_scale_ratio() of each axis. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmScale1Args {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u32 scaled_width;
    vmaf_mtl_u32 scaled_height;
    float ratio_x;
    float ratio_y;
} VmafMtlFmScale1Args;

/* Arguments of the row kernel: the plane of the differences, and where its
 * first row sum goes in the read-back. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmRowArgs {
    vmaf_mtl_u32 width;
    vmaf_mtl_u32 height;
    vmaf_mtl_u32 first_row;
} VmafMtlFmRowArgs;

/* motion_tools.h FILTER_5_s, FILTER_3_s (in the middle of five taps) and
 * FILTER_5_NO_OP_s. The literals round to the CPU's floats; the host test
 * compares them bit for bit. */
VMAF_MTL_CONSTANT float vmaf_mtl_fm_filter5[VMAF_MTL_FM_TAPS] = {
    0.054488685f, 0.244201342f, 0.402619947f, 0.244201342f, 0.054488685f,
};
VMAF_MTL_CONSTANT float vmaf_mtl_fm_filter3[VMAF_MTL_FM_TAPS] = {
    0.0f, 0.166378498f, 0.667243004f, 0.166378498f, 0.0f,
};
VMAF_MTL_CONSTANT float vmaf_mtl_fm_filter_no_op[VMAF_MTL_FM_TAPS] = {
    0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
};

// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmTaps {
    float w[VMAF_MTL_FM_TAPS];
} VmafMtlFmTaps;

/* The 5x5 neighbourhood of one output sample, row-major: v[r * 5 + c] is
 * the sample at row y - 2 + r and column x - 2 + c, both mirrored with
 * vmaf_mtl_fm_reflect101(). */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmWindow {
    float v[VMAF_MTL_FM_TAPS * VMAF_MTL_FM_TAPS];
} VmafMtlFmWindow;

/* The four samples motion_bilinear_interp() weighs: rows y1 / y2, columns
 * x1 / x2. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmCorners {
    float s11;
    float s12;
    float s21;
    float s22;
} VmafMtlFmCorners;

/* Where motion_bilinear_interp() reads for one half-size sample. */
// NOLINTNEXTLINE(modernize-use-using): C and MSL share this header, ADR-1498
typedef struct VmafMtlFmBilinearAt {
    vmaf_mtl_i32 x1;
    vmaf_mtl_i32 x2;
    vmaf_mtl_i32 y1;
    vmaf_mtl_i32 y2;
    float dx;
    float dy;
} VmafMtlFmBilinearAt;

/* 1 / scaler of picture_copy(): 4, 16 and 256 for 10, 12 and 16 bits, 1
 * otherwise. Powers of two, so a product with it is picture_copy()'s
 * quotient exactly. */
VMAF_MTL_FUNC float vmaf_mtl_fm_inv_scaler(vmaf_mtl_u32 bpc)
{
    if (bpc == 10u) {
        return 0.25f;
    }
    if (bpc == 12u) {
        return 0.0625f;
    }
    return (bpc == 16u) ? 0.00390625f : 1.0f;
}

/* picture_copy() of one sample with the offset -128 float_motion.c gives it:
 * raw / scaler + -128. */
VMAF_MTL_FUNC float vmaf_mtl_fm_sample(vmaf_mtl_u32 raw, float inv_scaler)
{
    const float scaled = (float)raw * inv_scaler;
    return scaled + -128.0f;
}

/* The filter of float_motion.c::motion_blur_plane() for motion_filter_size:
 * 1 is the no-op, 3 the 3-tap, every other value the 5-tap. A zero outer
 * weight times a finite sample adds 0, so the 3-tap rows of five are the
 * CPU's 3-tap sums. */
VMAF_MTL_FUNC VmafMtlFmTaps vmaf_mtl_fm_taps(vmaf_mtl_u32 filter_size)
{
    VmafMtlFmTaps t;
    for (int k = 0; k < VMAF_MTL_FM_TAPS; k++) {
        if (filter_size == 1u) {
            t.w[k] = vmaf_mtl_fm_filter_no_op[k];
        } else if (filter_size == 3u) {
            t.w[k] = vmaf_mtl_fm_filter3[k];
        } else {
            t.w[k] = vmaf_mtl_fm_filter5[k];
        }
    }
    return t;
}

/* convolution_reflect101() of common/convolution_internal.h: the index
 * folded into [0, size) without repeating the edge sample. The iterated fold
 * has period 2 * (size - 1) and is symmetric about 0, so this is its closed
 * form, for every index and every size (the host test compares the two). */
VMAF_MTL_FUNC vmaf_mtl_i32 vmaf_mtl_fm_reflect101(vmaf_mtl_i32 idx, vmaf_mtl_i32 size)
{
    if (size <= 1) {
        return 0;
    }
    const vmaf_mtl_i32 period = 2 * (size - 1);
    const vmaf_mtl_i32 m = ((idx < 0) ? -idx : idx) % period;
    return (m < size) ? m : period - m;
}

/* One output sample of convolution_f32_c_s(): the vertical pass of each of
 * the window's five columns, then the horizontal pass over the five column
 * values. Each pass starts from 0 and adds tap k's product in tap order,
 * every product and every sum rounded to fp32: convolution_edge_s(), the
 * interior loops of convolution_y_c_s() / convolution_x_c_s() and the AVX2
 * scanlines alike. */
VMAF_MTL_FUNC float vmaf_mtl_fm_blur(VmafMtlFmTaps t, const VMAF_MTL_FM_THR VmafMtlFmWindow *win)
{
    float col[VMAF_MTL_FM_TAPS];
    for (int c = 0; c < VMAF_MTL_FM_TAPS; c++) {
        float accum = 0.0f;
        for (int r = 0; r < VMAF_MTL_FM_TAPS; r++) {
            const float product = t.w[r] * win->v[r * VMAF_MTL_FM_TAPS + c];
            accum += product;
        }
        col[c] = accum;
    }
    float blurred = 0.0f;
    for (int c = 0; c < VMAF_MTL_FM_TAPS; c++) {
        const float product = t.w[c] * col[c];
        blurred += product;
    }
    return blurred;
}

/* Where sample (x, y) sits in a transposed difference plane of `width`
 * columns: the group of VMAF_MTL_FM_ROW_GROUP rows, then the column, then the
 * row within the group. */
VMAF_MTL_FUNC vmaf_mtl_u32 vmaf_mtl_fm_diff_index(vmaf_mtl_u32 x, vmaf_mtl_u32 y,
                                                  vmaf_mtl_u32 width)
{
    const vmaf_mtl_u32 group = y / VMAF_MTL_FM_ROW_GROUP;
    const vmaf_mtl_u32 lane = y % VMAF_MTL_FM_ROW_GROUP;
    return ((group * width) + x) * VMAF_MTL_FM_ROW_GROUP + lane;
}

/* One term of float_sad_line(): `diff < 0 ? -diff : diff` of two samples. */
VMAF_MTL_FUNC float vmaf_mtl_fm_abs_diff(float cur, float prev)
{
    const float diff = cur - prev;
    return (diff < 0.0f) ? -diff : diff;
}

/* motion_scale_bilinear()'s ratio of one axis: (float)src / (float)dst. */
VMAF_MTL_FUNC float vmaf_mtl_fm_scale_ratio(vmaf_mtl_u32 extent, vmaf_mtl_u32 scaled_extent)
{
    return (float)extent / (float)scaled_extent;
}

/* motion_scale_bilinear()'s source coordinate of half-size sample `i`:
 * (i + 0.5f) * ratio - 0.5f. */
VMAF_MTL_FUNC float vmaf_mtl_fm_scale_coord(vmaf_mtl_u32 i, float ratio)
{
    const float centre = (float)i + 0.5f;
    const float scaled = centre * ratio;
    return scaled - 0.5f;
}

/* motion.c::motion_mirror_f() with a left edge of 0. */
VMAF_MTL_FUNC float vmaf_mtl_fm_mirror(float i, float right)
{
    if (i < 0.0f) {
        return -i;
    }
    if (i > right) {
        const float twice = 2.0f * right;
        return twice - i;
    }
    return i;
}

/* Where motion_bilinear_interp() reads for half-size sample (x, y) of a
 * `width` x `height` blurred plane: (int)motion_mirror_f() of floorf() and
 * ceilf() of each coordinate, and the fractions from the lower indices. */
VMAF_MTL_FUNC VmafMtlFmBilinearAt vmaf_mtl_fm_bilinear_at(vmaf_mtl_u32 width, vmaf_mtl_u32 height,
                                                          float ratio_x, float ratio_y,
                                                          vmaf_mtl_u32 x, vmaf_mtl_u32 y)
{
    const float xx = vmaf_mtl_fm_scale_coord(x, ratio_x);
    const float yy = vmaf_mtl_fm_scale_coord(y, ratio_y);
    const float right = (float)(width - 1u);
    const float bottom = (float)(height - 1u);
    VmafMtlFmBilinearAt at;
    at.x1 = (vmaf_mtl_i32)vmaf_mtl_fm_mirror(VMAF_MTL_FM_FLOOR(xx), right);
    at.x2 = (vmaf_mtl_i32)vmaf_mtl_fm_mirror(VMAF_MTL_FM_CEIL(xx), right);
    at.y1 = (vmaf_mtl_i32)vmaf_mtl_fm_mirror(VMAF_MTL_FM_FLOOR(yy), bottom);
    at.y2 = (vmaf_mtl_i32)vmaf_mtl_fm_mirror(VMAF_MTL_FM_CEIL(yy), bottom);
    at.dx = xx - (float)at.x1;
    at.dy = yy - (float)at.y1;
    return at;
}

/* motion_bilinear_interp() of four samples, every operand in the CPU's
 * order: (1-dy)(1-dx) s11 + (1-dy) dx s12 + dy (1-dx) s21 + dy dx s22, the
 * weights formed left to right and the terms added left to right. */
VMAF_MTL_FUNC float vmaf_mtl_fm_bilinear(VmafMtlFmCorners s, float dx, float dy)
{
    const float wy = 1.0f - dy;
    const float wx = 1.0f - dx;
    const float w11 = wy * wx;
    const float t11 = w11 * s.s11;
    const float w12 = wy * dx;
    const float t12 = w12 * s.s12;
    const float w21 = dy * wx;
    const float t21 = w21 * s.s21;
    const float w22 = dy * dx;
    const float t22 = w22 * s.s22;
    const float sum12 = t11 + t12;
    const float sum123 = sum12 + t21;
    return sum123 + t22;
}

#if !defined(__METAL_VERSION__)

/* Host side: the sizes the Objective-C++ allocates and the tail it runs on
 * the read-back row sums. Not compiled into a kernel. */

#include <stddef.h>

#include "../float_motion_sad.h"

/* NOLINTBEGIN(modernize-use-nullptr): also a C header. The fork builds C as
 * C23, where clang-tidy proposes `nullptr`, but the required MSVC C build does
 * not provide that keyword. Preserve the portable C spelling. ADR-1138. */

/* Floats of the transposed difference plane of a `width` x `height` plane:
 * whole groups of rows. The rows past `height` in the last group are never
 * written or read. */
static inline uint64_t vmaf_mtl_fm_diff_count(unsigned width, unsigned height)
{
    const uint64_t groups = ((uint64_t)height + VMAF_MTL_FM_ROW_GROUP - 1u) / VMAF_MTL_FM_ROW_GROUP;
    return groups * width * VMAF_MTL_FM_ROW_GROUP;
}

/* motion.c::vmaf_image_sad_c()'s half-size extent: (int)(extent * 0.5 + 0.5). */
static inline unsigned vmaf_mtl_fm_scaled_extent(unsigned extent)
{
    // NOLINTNEXTLINE(bugprone-incorrect-roundings): the reference's own (int)(width * 0.5 + 0.5), motion.c::vmaf_image_sad_c(), ADR-1498
    return (unsigned)(((double)extent * 0.5) + 0.5);
}

/* The score of one plane from its row sums, as motion.c::vmaf_image_sad_c()
 * returns it: the fp32 mean of the scale-0 row sums, plus the fp32 mean of
 * the scale-1 row sums in fp32 when `scale1_rows` is not NULL
 * (motion_add_scale1). Without the option this is
 * float_motion.c::compute_motion_simd(). */
static inline double vmaf_mtl_fm_plane_score(const float *rows, unsigned width, unsigned height,
                                             const float *scale1_rows, unsigned scaled_width,
                                             unsigned scaled_height)
{
    float score = (float)vmaf_float_motion_score_from_row_sads(rows, width, height);
    if (scale1_rows != NULL) {
        score +=
            (float)vmaf_float_motion_score_from_row_sads(scale1_rows, scaled_width, scaled_height);
    }
    return (double)score;
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* !defined(__METAL_VERSION__) */

// cppcheck-suppress-end passedByValue

#endif /* VMAF_FEATURE_METAL_METAL_FLOAT_MOTION_MATH_H_ */
