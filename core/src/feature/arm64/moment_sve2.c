/**
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 *  aarch64 SVE2 port of compute_1st_moment / compute_2nd_moment for the
 *  float_moment feature extractor (ADR-0584).
 *
 *  Bit-exactness contract (ADR-0138 / ADR-0179):
 *  Per-row accumulation uses svfloat64_t so every f32 sample is widened to
 *  f64 before summation, bounding the cross-lane ULP error to the double
 *  precision range.  The pattern is vector-length-agnostic (VLA): the inner
 *  loop steps by `svcntd()` f32 elements per iteration (equal to the number
 *  of f64 lanes per register), using `svwhilelt_b64` to construct a predicate
 *  that covers exactly those elements and `svcvt_f64_f32_x` to widen them.
 *
 *  Lane mapping of the widening converts (CRITICAL — see ARM A64 reference):
 *    The SVE `FCVT .S -> .D` (svcvt_f64_f32) does NOT compact the lower
 *    `svcntd()` contiguous f32 lanes into the f64 lanes.  Per the A64
 *    pseudocode, destination f64 element `e` reads source f32 element
 *    `2*e` — the EVEN-indexed f32 lane sitting in the low half of each
 *    64-bit container.  The odd-indexed (top-half) f32 lanes are read by
 *    the SVE2 `FCVTLT .S -> .D` (svcvtlt_f64_f32), which maps f64 element
 *    `e` to f32 element `2*e + 1`.
 *
 *    An earlier implementation here stepped by `svcntd()` and used only
 *    `svcvt_f64_f32_x`, assuming it widened the lower contiguous lanes.  On
 *    any SVE register wider than 64 bits that silently summed the even f32
 *    lanes twice and dropped every odd lane, producing a wrong moment.  The
 *    correct VLA pattern processes a full f32 register (`svcntw()` lanes) per
 *    iteration and widens BOTH halves:
 *      - even lanes via svcvt_f64_f32_x   (f32[2e]   -> f64[e])
 *      - odd  lanes via svcvtlt_f64_f32_x (f32[2e+1] -> f64[e])
 *    Merging adds (`svadd_f64_m`) accumulate the active f64 lanes while
 *    leaving the running `dsum` untouched on inactive (tail) lanes.
 *
 *  Why `svadd_f64_m` (merging) and not `_x`:
 *    The `_x` (don't-care) variant leaves inactive lanes UNDEFINED, which
 *    would corrupt `dsum` on the partial tail iteration and feed garbage into
 *    the final `svaddv_f64(svptrue_b64(), dsum)`.  Merging preserves the
 *    inactive lanes of `dsum`, so only validly-converted elements contribute.
 *
 *  FCVTLT requires FEAT_SVE2, which is exactly this TU's build gate
 *  (-march=...+sve2 / HAVE_SVE2), so no extra feature check is needed.
 *
 *  Darwin opt-out: ADR-0419.  The runtime gate in arm/cpu.c is
 *  `__linux__`-gated so VMAF_ARM_CPU_FLAG_SVE2 is never set on Apple Silicon
 *  regardless of chip capability.  The meson build gate mirrors
 *  `is_sve2_supported` which is forced false on Darwin (ADR-0419).
 */

#include <arm_sve.h>
#include <assert.h>
#include <stddef.h>

#include "moment_sve2.h"

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunknown-pragmas"
#endif
#pragma STDC FP_CONTRACT OFF
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

int compute_1st_moment_sve2(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    /* Step by a full f32 register (svcntw()): each iteration widens BOTH the
     * even (svcvt) and odd (svcvtlt) f32 lanes of the loaded register. */
    const int step = (int)svcntw();
    double cum = 0.0;

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        svfloat64_t dsum = svdup_f64(0.0);
        int j = 0;

        while (j < w) {
            /* Contiguous f32 load predicate: active for min(svcntw(), w-j)
             * elements starting at row[j].  Inactive lanes load as zero but
             * are never converted (the b64 predicates below exclude them). */
            const svbool_t pg32 = svwhilelt_b32((uint32_t)j, (uint32_t)w);
            const svfloat32_t vf32 = svld1_f32(pg32, row + j);
            /* n = number of valid contiguous f32 elements in this window. */
            const uint64_t n = svcntp_b32(svptrue_b32(), pg32);
            /* Even f32 lanes -> low f64 lanes (FCVT): ceil(n/2) of them. */
            const svbool_t pe = svwhilelt_b64((uint64_t)0, (n + 1) / 2);
            /* Odd  f32 lanes -> low f64 lanes (FCVTLT): floor(n/2) of them. */
            const svbool_t po = svwhilelt_b64((uint64_t)0, n / 2);
            const svfloat64_t v_even = svcvt_f64_f32_x(pe, vf32);
            const svfloat64_t v_odd = svcvtlt_f64_f32_x(po, vf32);
            dsum = svadd_f64_m(pe, dsum, v_even);
            dsum = svadd_f64_m(po, dsum, v_odd);
            j += step;
        }

        cum += svaddv_f64(svptrue_b64(), dsum);
    }

    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}

int compute_2nd_moment_sve2(const float *pic, int w, int h, int stride, double *score)
{
    assert(pic != NULL);
    assert(score != NULL);
    assert(w > 0);
    assert(h > 0);

    const int stride_f = stride / (int)sizeof(float);
    /* Step by a full f32 register (svcntw()); widen both halves per iteration
     * (see compute_1st_moment_sve2 for the FCVT/FCVTLT lane-mapping rationale). */
    const int step = (int)svcntw();
    double cum = 0.0;

    for (int i = 0; i < h; ++i) {
        const float *row = pic + (size_t)i * (size_t)stride_f;
        svfloat64_t dsum = svdup_f64(0.0);
        int j = 0;

        while (j < w) {
            const svbool_t pg32 = svwhilelt_b32((uint32_t)j, (uint32_t)w);
            const svfloat32_t vf32 = svld1_f32(pg32, row + j);
            /* Square in f32 before widening: mirrors the scalar reference
             * (moment.c: `pic_ * pic_`) and the NEON sibling, both of which
             * square in f32 then widen to f64 (ADR-0179). */
            const svfloat32_t vsq = svmul_f32_x(pg32, vf32, vf32);
            const uint64_t n = svcntp_b32(svptrue_b32(), pg32);
            /* Even f32 lanes -> low f64 lanes (FCVT); odd via FCVTLT. */
            const svbool_t pe = svwhilelt_b64((uint64_t)0, (n + 1) / 2);
            const svbool_t po = svwhilelt_b64((uint64_t)0, n / 2);
            const svfloat64_t v_even = svcvt_f64_f32_x(pe, vsq);
            const svfloat64_t v_odd = svcvtlt_f64_f32_x(po, vsq);
            dsum = svadd_f64_m(pe, dsum, v_even);
            dsum = svadd_f64_m(po, dsum, v_odd);
            j += step;
        }

        cum += svaddv_f64(svptrue_b64(), dsum);
    }

    cum /= (double)w * (double)h;
    *score = cum;
    return 0;
}
