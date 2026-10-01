/**
 *
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: EUPL-1.2
 */

/*
 * Host tail of calc_psnrhvs() for the GPU twins (ADR-1397); see
 * psnr_hvs_score.h. Built with the strict floating-point arguments of the
 * scalar reference (libvmaf_psnr_hvs_scalar in core/src/meson.build), so the
 * combined score is not contracted into a fused multiply-add on hosts that
 * have one.
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stddef.h>
#include <stdint.h>

#include "feature/psnr_hvs_score.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but this is a C
 * translation unit whose sources spell the null pointer constant `NULL` and
 * MSVC's documented /std:clatest C23 feature set does not include `nullptr`
 * while the required Windows build compiles this TU with cl.exe. ADR-1138. */

double vmaf_psnr_hvs_plane_score(const float *terms, size_t n_blocks, unsigned bpc)
{
    if (terms == NULL || n_blocks == 0u || bpc == 0u || bpc > VMAF_PSNR_HVS_MAX_BPC)
        return (double)NAN;
    if (n_blocks > (size_t)INT_MAX / VMAF_PSNR_HVS_TERMS_PER_BLOCK)
        return (double)NAN;

    const size_t n_terms = n_blocks * VMAF_PSNR_HVS_TERMS_PER_BLOCK;
    /* The guards above keep the count inside the CPU's `int pixels`. */
    assert(n_terms / VMAF_PSNR_HVS_TERMS_PER_BLOCK == n_blocks);
    assert(n_terms <= (size_t)INT_MAX);
    /* calc_psnrhvs(): `ret += (err * csf) * (err * csf)` once per coefficient,
     * `ret` a float. The order and the type are the contract: the rounding of
     * every addition depends on the sum so far. Do not vectorise, reassociate
     * or widen this loop. */
    float ret = 0.0f;
    for (size_t i = 0; i < n_terms; i++)
        ret += terms[i];

    /* `ret /= pixels` and `ret /= samplemax * samplemax`, both int operands
     * converted to float. */
    const int pixels = (int)n_terms;
    ret /= (float)pixels;
    const int32_t samplemax = (int32_t)((1u << bpc) - 1u);
    ret /= (float)(samplemax * samplemax);
    return (double)ret;
}

double vmaf_psnr_hvs_combined_score(const double *plane_scores, unsigned n_planes)
{
    if (plane_scores == NULL)
        return (double)NAN;
    if (n_planes == 1u)
        return plane_scores[0];
    if (n_planes != 3u)
        return (double)NAN;
    return (plane_scores[0]) * .8 + .1 * (plane_scores[1] + plane_scores[2]);
}

double vmaf_psnr_hvs_score_db(double score)
{
    return 10 * (-1 * log10(1.0 * score));
}

/* NOLINTEND(modernize-use-nullptr) */
