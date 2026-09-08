/* Copyright 2026 Lusoris
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 * Private vmaf-roi input and placeholder helpers, shared with boundary tests.
 * Header-only like vmaf_roi_core.h; no libvmaf API or duplicate CLI is exposed.
 */
#ifndef LIBVMAF_TOOLS_VMAF_ROI_INPUT_H_
#define LIBVMAF_TOOLS_VMAF_ROI_INPUT_H_

/* NOLINTBEGIN(modernize-use-nullptr) -- ADR-1138: preserve C/upstream NULL
 * compatibility; required Windows MSVC /std:clatest does not document nullptr. */
#include <errno.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

/* Existing CLI dimension limit; bounds all input and saliency allocations. */
#define VMAF_ROI_MAX_DIM 16384

static inline size_t luma_plane_size(int w, int h)
{
    if (w <= 0 || h <= 0 || w > VMAF_ROI_MAX_DIM || h > VMAF_ROI_MAX_DIM)
        return 0U;
    return (size_t)w * (size_t)h;
}

static inline int read_luma8(FILE *fp, uint8_t *dst, size_t y_sz, int bitdepth, size_t *got_bytes)
{
    if (got_bytes == NULL)
        return -EINVAL;
    *got_bytes = 0U;
    if (fp == NULL || dst == NULL || y_sz == 0U ||
        y_sz > (size_t)VMAF_ROI_MAX_DIM * (size_t)VMAF_ROI_MAX_DIM ||
        (bitdepth != 8 && bitdepth != 10 && bitdepth != 12 && bitdepth != 16))
        return -EINVAL;
    if (bitdepth == 8) {
        size_t got = fread(dst, 1U, y_sz, fp);
        *got_bytes = got;
        if (got != y_sz)
            return -EIO;
        return 0;
    }

    uint8_t *raw = (uint8_t *)malloc(y_sz * 2U);
    if (raw == NULL)
        return -ENOMEM;
    size_t got = fread(raw, 2U, y_sz, fp);
    *got_bytes = got * 2U;
    if (got != y_sz) {
        free(raw);
        return -EIO;
    }

    const unsigned shift = (unsigned)bitdepth - 8U;
    const unsigned round = (shift == 0U) ? 0U : (1U << (shift - 1U));
    const unsigned max_sample = (1U << (unsigned)bitdepth) - 1U;
    for (size_t i = 0U; i < y_sz; ++i) {
        unsigned v = (unsigned)raw[i * 2U] | ((unsigned)raw[i * 2U + 1U] << 8U);
        if (v > max_sample)
            v = max_sample;
        const unsigned luma8 = (v + round) >> shift;
        dst[i] = (uint8_t)((luma8 > UINT8_MAX) ? UINT8_MAX : luma8);
    }
    free(raw);
    return 0;
}

/* Center-weighted radial placeholder. Returns saliency in [0, 1]: 1 at the
 * frame centre, falling off to ~0 in the corners. Only used for smoke
 * testing the sidecar plumbing -- NOT a substitute for MobileSal. */
static inline int fill_placeholder_saliency(int w, int h, float *dst, size_t n)
{
    if (dst == NULL || n == 0U || n != luma_plane_size(w, h))
        return -EINVAL;
    const double cx = (double)(w - 1) * 0.5;
    const double cy = (double)(h - 1) * 0.5;
    const double rmax = sqrt(cx * cx + cy * cy);
    const double inv_rmax = (rmax > 0.0) ? (1.0 / rmax) : 0.0;
    /* Bound the traversal by the caller's allocation count. Coordinate
     * arithmetic and evaluation order match the original row-major loop. */
    for (size_t i = 0U; i < n; ++i) {
        const size_t row = i / (size_t)w;
        const double dy = (double)row - cy;
        const double dx = (double)(i % (size_t)w) - cx;
        const double r = sqrt(dx * dx + dy * dy) * inv_rmax;
        const double s = 1.0 - r;
        dst[i] = (float)((s < 0.0) ? 0.0 : s);
    }
    return 0;
}

/* NOLINTEND(modernize-use-nullptr) -- ADR-1138 */
#endif /* LIBVMAF_TOOLS_VMAF_ROI_INPUT_H_ */
