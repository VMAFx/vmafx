/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Shared numerical contract and host tail of float_motion.c for the GPU twins
 * (ADR-1403, ADR-1409, ADR-1411).
 *
 * Both steps are the CPU's arithmetic, so each twin returns the CPU
 * extractor's score bit for bit:
 *  - the blur is convolution_f32_c_s(): each tap one rounded fp32 multiply
 *    and one rounded fp32 add, taps in order, vertical pass then horizontal.
 *    Each backend translation unit is built with contraction off, so nothing
 *    is fused.
 *  - the SAD is compute_motion_simd(): float_sad_line() adds the absolute
 *    differences of one row, left to right, into one fp32 accumulator, and
 *    the rows are added top to bottom into another. The result depends on
 *    that order (at 1920x1080 it is up to 1.4e-4 from the exact sum), so
 *    float_motion_row_sad runs one thread per row over the whole row, and
 *    the host adds the rows with vmaf_float_motion_score_from_row_sads()
 *    below. A per-block partial sum cannot give that value.
 */

#ifndef FEATURE_FLOAT_MOTION_SAD_H_
#define FEATURE_FLOAT_MOTION_SAD_H_

#ifndef VMAF_FLOAT_MOTION_FUNC
#define VMAF_FLOAT_MOTION_FUNC static inline
#endif

/*
 * Bit-depth scaler for float_motion reference conversion:
 *   val = (raw / scaler) - 128.0f
 *
 * Maps bits per component (bpc) to scaler factor:
 *   8  -> 1.0f
 *   10 -> 4.0f
 *   12 -> 16.0f
 *   16 -> 256.0f
 * Default: 1.0f.
 */
VMAF_FLOAT_MOTION_FUNC float vmaf_float_motion_scaler(unsigned bpc)
{
    float scaler = 1.0f;
    if (bpc == 10) {
        scaler = 4.0f;
    } else if (bpc == 12) {
        scaler = 16.0f;
    } else if (bpc == 16) {
        scaler = 256.0f;
    }
    return scaler;
}

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
