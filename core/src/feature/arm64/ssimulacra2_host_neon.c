/**
 *
 *  Copyright (c) the JPEG XL Project Authors.
 *  Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent AND BSD-3-Clause
 */

/*
 * aarch64 NEON host-kernel variants for the ssimulacra2 Vulkan extractor
 * (ADR-0242). 4-wide float lanes; structurally mirrors the AVX2 sibling
 * (ssimulacra2_host_avx2.c) and the standalone NEON kernels in
 * ssimulacra2_neon.c.
 *
 * The only difference from `ssimulacra2_linear_rgb_to_xyb_neon` is the
 * `plane_stride` parameter: channel pointers are `base + p * plane_stride`
 * instead of `base + p * w*h`.
 *
 * Bit-exact contract: ADR-0161 / ADR-0242 — per-lane scalar cbrtf,
 * `#pragma STDC FP_CONTRACT OFF` (gated by a -Wunknown-pragmas push
 * so older GCC keeps quiet), compiled with `-ffp-contract=off`.
 */

#include <arm_neon.h>
#include <assert.h>
#include <math.h>
#include <stdalign.h>
#include <stddef.h>
#include <stdint.h>

#include "feature/ssimulacra2_math.h"
#include "ssimulacra2_host_neon.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#if !defined(_MSC_VER) /* cl.exe has no contraction pragma; /fp:precise does not contract */
#pragma STDC FP_CONTRACT OFF
#endif
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "ssimulacra2_arm64_common.h"

void ssimulacra2_host_linear_rgb_to_xyb_neon(const float *lin, float *xyb, unsigned w, unsigned h,
                                             size_t plane_stride)
{
    assert(lin != NULL);
    assert(xyb != NULL);
    assert(w > 0 && h > 0);
    assert(plane_stride >= (size_t)w * (size_t)h);

    const float *rp = lin;
    const float *gp = lin + plane_stride;
    const float *bp = lin + 2u * plane_stride;
    float *xp = xyb;
    float *yp = xyb + plane_stride;
    float *bxp = xyb + 2u * plane_stride;
    const Ss2XybK k = ss2_xyb_k_init();
    const size_t pixels = (size_t)w * (size_t)h;

    size_t i = 0;
    for (; i + 4 <= pixels; i += 4) {
        ss2_xyb_block_neon(&k, rp + i, gp + i, bp + i, xp + i, yp + i, bxp + i);
    }
    for (; i < pixels; i++) {
        ss2_xyb_pixel(&k, rp[i], gp[i], bp[i], xp + i, yp + i, bxp + i);
    }
}

void ssimulacra2_host_downsample_2x2_neon(const float *in, unsigned iw, unsigned ih, float *out,
                                          unsigned ow, unsigned oh, size_t plane_stride)
{
    assert(in != NULL);
    assert(out != NULL);
    assert(iw > 0 && ih > 0);
    assert(plane_stride >= (size_t)iw * (size_t)ih);

    for (int c = 0; c < 3; c++) {
        const float *ip = in + (size_t)c * plane_stride;
        float *op = out + (size_t)c * plane_stride;
        for (unsigned oy = 0; oy < oh; oy++) {
            const unsigned iy0 = oy * 2;
            const unsigned iy1 = (iy0 + 1 < ih) ? iy0 + 1 : ih - 1;
            ss2_downsample_row_2x2_neon(ip + (size_t)iy0 * iw, ip + (size_t)iy1 * iw,
                                        op + (size_t)oy * ow, iw, ow);
        }
    }
}
