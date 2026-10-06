/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * The CPU reference of the RGB input conversion (RC4 WP13, ADR-2146): the
 * plan of a conversion from the stated matrix and ranges, and the plane
 * reader the host import and the vmaf command line call and the device twins
 * are held to. Header only, plain C, no engine dependency (like
 * import_layout.h); the coefficients are generated from the matrices of
 * core/api/vmafx.toml (rgb_coefficients_gen.h) and the per-pixel arithmetic
 * is rgb_math.h. The refusal of a statement that is missing or names
 * something the integer reference does not convert is
 * vmafx_rgb_check_statement() (internal.h, rgb_convert.c).
 */

#ifndef VMAF_SRC_VMAFX_RGB_CONVERT_H_
#define VMAF_SRC_VMAFX_RGB_CONVERT_H_

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "vmafx/import_convert.h"
#include "vmafx/import_layout.h"
#include "vmafx/rgb_coefficients_gen.h"
#include "vmafx/rgb_math.h"

/* NOLINTBEGIN(modernize-use-nullptr): C header included by C translation
 * units; MSVC's C mode has no `nullptr`. ADR-1138. */

_Static_assert(VMAFX_RGB_Q == VMAFX_RGB_SHIFT, "the generated table and rgb_math.h share Q");

/* VmafxColorRange values the plan reads (vmafx/types.h): LIMITED 1, FULL 2. */
#define VMAFX_RGB_RANGE_LIMITED 1u
#define VMAFX_RGB_RANGE_FULL 2u
/* Black level, in units of 2^(bpc - 8), of the limited range. */
#define VMAFX_RGB_LIMITED_BLACK 16

/* The plan of `layout` at `bpc` bits for the matrix (a VmafxColorMatrix) and
 * the input and output ranges (VmafxColorRange) of a checked descriptor;
 * false when the matrix has no conversion. */
static inline bool vmafx_rgb_plan_init(VmafxRgbPlan *plan, const VmafxImportLayout *layout,
                                       uint32_t bpc, uint32_t matrix, uint32_t in_range,
                                       uint32_t out_range)
{
    assert(plan != NULL && layout != NULL && bpc >= 8u && bpc <= 16u);
    if (matrix >= VMAFX_RGB_N_MATRICES || in_range < VMAFX_RGB_RANGE_LIMITED ||
        in_range > VMAFX_RGB_RANGE_FULL || out_range < VMAFX_RGB_RANGE_LIMITED ||
        out_range > VMAFX_RGB_RANGE_FULL) {
        return false;
    }
    const int64_t (*const m)[3] =
        vmafx_rgb_coefficients[matrix][in_range - 1u][out_range - 1u][bpc - 8u];
    const int64_t luma_sum = m[0][0] + m[0][1] + m[0][2];
    if (luma_sum == 0) {
        return false;
    }
    for (uint32_t row = 0; row < 3u; row++) {
        for (uint32_t col = 0; col < 3u; col++) {
            plan->coef[row][col] = m[row][col];
        }
    }
    const int64_t unit = (int64_t)1 << (bpc - 8u);
    const int64_t off_in = in_range == VMAFX_RGB_RANGE_LIMITED ? VMAFX_RGB_LIMITED_BLACK * unit : 0;
    const int64_t off_out =
        out_range == VMAFX_RGB_RANGE_LIMITED ? VMAFX_RGB_LIMITED_BLACK * unit : 0;
    const int64_t half = (int64_t)1 << (VMAFX_RGB_SHIFT - 1u);
    const int64_t mid = (int64_t)1 << (bpc - 1u);
    plan->add[0] = (off_out << VMAFX_RGB_SHIFT) + half - luma_sum * off_in;
    plan->add[1] = (mid << VMAFX_RGB_SHIFT) + half;
    plan->add[2] = plan->add[1];
    plan->max = (uint32_t)(((uint32_t)1u << bpc) - 1u);
    plan->elems = layout->rgb_elems;
    plan->in_bytes = bpc > 8u ? 2u : 1u;
    for (uint32_t c = 0; c < 3u; c++) {
        plan->pos[c] = layout->elem[c];
    }
    return true;
}

static inline void vmafx_rgb_read_plane(uint8_t *dst, size_t dst_stride, const uint8_t *src,
                                        size_t src_pitch, unsigned w, unsigned h, uint32_t plane,
                                        const VmafxRgbPlan *plan)
{
    assert(plane < 3u && (plan->in_bytes == 1u || plan->in_bytes == 2u));
    for (unsigned y = 0u; y < h; y++) {
        uint8_t *const d = dst + (size_t)y * dst_stride;
        const uint8_t *const s = src + (size_t)y * src_pitch;
        for (unsigned x = 0u; x < w; x++) {
            const size_t base = (size_t)x * plan->elems;
            const uint32_t r =
                vmafx_import_load(s, (base + plan->pos[0]) * plan->in_bytes, plan->in_bytes);
            const uint32_t g =
                vmafx_import_load(s, (base + plan->pos[1]) * plan->in_bytes, plan->in_bytes);
            const uint32_t b =
                vmafx_import_load(s, (base + plan->pos[2]) * plan->in_bytes, plan->in_bytes);
            const uint32_t v = vmafx_rgb_value(plan, plane, r, g, b);
            if (plan->in_bytes == 1u) {
                d[x] = (uint8_t)v;
            } else {
                d[2u * (size_t)x] = (uint8_t)v;
                d[2u * (size_t)x + 1u] = (uint8_t)(v >> 8u);
            }
        }
    }
}

/* NOLINTEND(modernize-use-nullptr) */

#endif /* VMAF_SRC_VMAFX_RGB_CONVERT_H_ */
