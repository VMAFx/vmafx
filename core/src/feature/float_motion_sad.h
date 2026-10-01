/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host tail of float_motion.c::compute_motion_simd() for the GPU twins
 * (ADR-1409).
 *
 * The CPU extractor adds the absolute differences of one row, left to right,
 * into one `float` (float_sad_line()), adds those row sums top to bottom into
 * another, and divides by the pixel count in `float`. The running sums round
 * at every step, so the score depends on the order: a twin that reduces in
 * any other shape differs from the CPU in the low bits (up to 1.4e-4 at
 * 1920x1080). A twin computes the row sums on the device in the CPU's order
 * and calls this helper for the rest.
 */

#ifndef FEATURE_FLOAT_MOTION_SAD_H_
#define FEATURE_FLOAT_MOTION_SAD_H_

/*
 * Score compute_motion_simd() returns, from its row sums.
 *
 * `row_sad` holds `h` values: row `i` is float_sad_line() of row `i` of the
 * two blurred planes. `w * h` is the pixel count, an `int` on the CPU, and
 * must fit one. An empty plane (`w` or `h` 0) gives NaN, never a score.
 */
static inline double vmaf_float_motion_score_from_row_sads(const float *row_sad, unsigned w,
                                                           unsigned h)
{
    float accum = 0.0f;
    for (unsigned i = 0; i < h; i++) {
        accum += row_sad[i];
    }
    return (double)(accum / (float)(int)(w * h));
}

#endif /* FEATURE_FLOAT_MOTION_SAD_H_ */
