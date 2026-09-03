/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  Pure helpers for vmaf-roi: per-CTU mean reducer + saliency-to-QP-offset
 *  mapper. Header-only so both the binary and the unit test compile their
 *  own copy without dragging libvmaf's full link surface into the test.
 *
 *  Stays in core/tools/ because it is private to vmaf-roi.
 */

#ifndef LIBVMAF_TOOLS_VMAF_ROI_CORE_H_
#define LIBVMAF_TOOLS_VMAF_ROI_CORE_H_

#include <math.h>
#include <stddef.h>
#include <stdint.h>

#define VMAF_ROI_CORE_QP_OFFSET_MAX 12

/* Compute the mean saliency over a single CTU's bounding box. Split out
 * to keep vmaf_roi_reduce_per_ctu() under the Power-of-10 nesting cap. */
static inline float vmaf_roi_ctu_mean(const float *sal, int w, int x0, int x1, int y0, int y1)
{
    double acc = 0.0;
    int n = 0;
    for (int y = y0; y < y1; ++y) {
        for (int x = x0; x < x1; ++x) {
            acc += (double)sal[(size_t)y * (size_t)w + (size_t)x];
            ++n;
        }
    }
    return (n > 0) ? (float)(acc / (double)n) : 0.0F;
}

/* Per-CTU mean reducer. cols / rows must be ceil(w/ctu) / ceil(h/ctu).
 * Partial CTUs at the right / bottom edge are averaged over their actual
 * sample count. */
static inline void vmaf_roi_reduce_per_ctu(const float *sal, int w, int h, int ctu, float *grid,
                                           int cols, int rows)
{
    for (int r = 0; r < rows; ++r) {
        const int y0 = r * ctu;
        const int y1 = (y0 + ctu < h) ? (y0 + ctu) : h;
        for (int c = 0; c < cols; ++c) {
            const int x0 = c * ctu;
            const int x1 = (x0 + ctu < w) ? (x0 + ctu) : w;
            grid[(size_t)r * (size_t)cols + (size_t)c] = vmaf_roi_ctu_mean(sal, w, x0, x1, y0, y1);
        }
    }
}

/* Saliency in [0, 1] -> signed QP offset, clamped to +-12.
 *
 *   qp = -strength * (2 * saliency - 1)
 *
 * High saliency (~1.0) -> -strength (preserve quality, lower QP).
 * Low saliency  (~0.0) -> +strength (save bits, higher QP).
 * Mid saliency (~0.5)  ->  0.       (neutral)
 */
static inline int vmaf_roi_saliency_to_qp(float saliency, double strength)
{
    const double v = -strength * ((double)saliency * 2.0 - 1.0);
    long rounded = lround(v);
    if (rounded > VMAF_ROI_CORE_QP_OFFSET_MAX)
        rounded = VMAF_ROI_CORE_QP_OFFSET_MAX;
    if (rounded < -VMAF_ROI_CORE_QP_OFFSET_MAX)
        rounded = -VMAF_ROI_CORE_QP_OFFSET_MAX;
    return (int)rounded;
}

#endif /* LIBVMAF_TOOLS_VMAF_ROI_CORE_H_ */
