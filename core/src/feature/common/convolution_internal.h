/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *
 *     Licensed under the BSD+Patent License (the "License");
 *     you may not use this file except in compliance with the License.
 *     You may obtain a copy of the License at
 *
 *         https://opensource.org/licenses/BSDplusPatent
 *
 *     Unless required by applicable law or agreed to in writing, software
 *     distributed under the License is distributed on an "AS IS" BASIS,
 *     WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *     See the License for the specific language governing permissions and
 *     limitations under the License.
 *
 */

#pragma once

#ifndef CONVOLUTION_INTERNAL_H_
#define CONVOLUTION_INTERNAL_H_

#include "macros.h"
#include <stdbool.h>

/**
 * Reflect-101 ("mirror without repeating the edge sample") index fold.
 *
 * Upstream carries a single-bounce form open-coded at every tap site:
 *
 *     if (idx < 0)          idx = -idx;
 *     else if (idx >= size) idx = size - (idx - size + 2);   // == 2*size - idx - 2
 *
 * One bounce is only sufficient when `size >= radius + 1`.  For a smaller
 * plane the bounced index lands outside the opposite edge and the caller
 * dereferences out of bounds: at size == 2 a tap of -2 folds to +2 (>= size)
 * and a tap of +3 folds to -1.  Reported upstream as Netflix/vmaf#1582 and
 * Netflix/vmaf#1581; reachable in this fork through `float_motion`'s
 * `motion_add_uv` chroma planes and through `float_vif`'s multi-scale ladder.
 *
 * Folding repeatedly until the index is in range is bit-identical to the
 * single bounce for every `size >= radius + 1` (the loop exits after the
 * first iteration), so no in-contract score moves.  `size <= 1` has no
 * interior to reflect into and would not terminate, so it short-circuits
 * to the only valid index.
 */
FORCE_INLINE int convolution_reflect101(int idx, int size)
{
    if (size <= 1)
        return 0;
    while (idx < 0 || idx >= size) {
        idx = (idx < 0) ? -idx : (2 * size - idx - 2);
    }
    return idx;
}

FORCE_INLINE float convolution_edge_s(bool horizontal, const float *filter, int filter_width,
                                      const float *src, int width, int height, int stride, int i,
                                      int j)
{
    int radius = filter_width / 2;

    float accum = 0;
    for (int k = 0; k < filter_width; ++k) {
        int i_tap = horizontal ? i : i - radius + k;
        int j_tap = horizontal ? j - radius + k : j;

        // Handle edges by mirroring (reflect-101).  The fold is iterative so
        // that planes smaller than radius + 1 stay in bounds; see
        // convolution_reflect101 above (Netflix/vmaf#1582).
        if (horizontal)
            j_tap = convolution_reflect101(j_tap, width);
        else
            i_tap = convolution_reflect101(i_tap, height);

        accum += filter[k] * src[i_tap * stride + j_tap];
    }
    return accum;
}

FORCE_INLINE float convolution_edge_sq_s(bool horizontal, const float *filter, int filter_width,
                                         const float *src, int width, int height, int stride, int i,
                                         int j)
{
    int radius = filter_width / 2;

    float accum = 0;
    float src_val;
    for (int k = 0; k < filter_width; ++k) {
        int i_tap = horizontal ? i : i - radius + k;
        int j_tap = horizontal ? j - radius + k : j;

        // Handle edges by mirroring (reflect-101).  The fold is iterative so
        // that planes smaller than radius + 1 stay in bounds; see
        // convolution_reflect101 above (Netflix/vmaf#1582).
        if (horizontal)
            j_tap = convolution_reflect101(j_tap, width);
        else
            i_tap = convolution_reflect101(i_tap, height);
        src_val = src[i_tap * stride + j_tap];
        accum += filter[k] * (src_val * src_val);
    }
    return accum;
}

FORCE_INLINE float convolution_edge_xy_s(bool horizontal, const float *filter, int filter_width,
                                         const float *src1, const float *src2, int width,
                                         int height, int stride1, int stride2, int i, int j)
{
    int radius = filter_width / 2;

    float accum = 0;
    float src_val1, src_val2;
    for (int k = 0; k < filter_width; ++k) {
        int i_tap = horizontal ? i : i - radius + k;
        int j_tap = horizontal ? j - radius + k : j;

        // Handle edges by mirroring (reflect-101).  The fold is iterative so
        // that planes smaller than radius + 1 stay in bounds; see
        // convolution_reflect101 above (Netflix/vmaf#1582).
        if (horizontal)
            j_tap = convolution_reflect101(j_tap, width);
        else
            i_tap = convolution_reflect101(i_tap, height);
        src_val1 = src1[i_tap * stride1 + j_tap];
        src_val2 = src2[i_tap * stride2 + j_tap];
        accum += filter[k] * (src_val1 * src_val2);
    }
    return accum;
}

/*
 * convolution_clamp_borders — bound a vertical/horizontal border split to the
 * plane so the border loops cannot run past, or before, the buffer.
 *
 * Shared by the scalar path and by every AVX2 / AVX-512 twin. The SIMD kernels
 * derive the same `radius` / `dim - radius` split and had the identical defect:
 * for a plane shorter than the filter radius, `dim - radius` goes negative, so
 * the trailing border loop starts at a negative row and the leading one runs
 * past the end. Both are heap writes, not just reads.
 */
static inline void convolution_clamp_borders(int dim, int *borders_lo, int *borders_hi)
{
    if (*borders_lo > dim)
        *borders_lo = dim;
    if (*borders_hi < *borders_lo)
        *borders_hi = *borders_lo;
}

#endif // CONVOLUTION_INTERNAL_H_
