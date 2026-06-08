/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
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

/*
 * FMA-safe NEON DWT2 kernel for float-ADM.
 *
 * Background (ADR-1057): The original `float_adm_dwt2_neon` in
 * float_adm_neon.c used `vmlaq_laneq_f32` intrinsics, which map
 * directly to AArch64 `fmla` instructions.  `fmla` performs a fused
 * multiply-accumulate with a single rounding, while the scalar
 * reference in adm_tools.c::adm_dwt2_s uses a separate multiply then
 * add (two roundings).  This produces a 1-ULP divergence on ARM CI
 * that `#pragma clang fp contract(off)` alone cannot suppress, because
 * compiler FP-contraction pragmas govern only compiler-generated
 * fusions of C-level `a*b+c` expressions — they have no effect on
 * intrinsics that directly encode FMA machine operations.
 *
 * Fix: replace every `vmlaq_laneq_f32(acc, v, coeff, lane)` with the
 * explicit two-step form `vaddq_f32(acc, vmulq_laneq_f32(v, coeff, lane))`.
 * The compiler emits separate `fmul` and `fadd` instructions, matching
 * the scalar reference rounding.
 *
 * The `#pragma clang fp contract(off)` / GCC attribute below are
 * belt-and-suspenders guards for the scalar tail: without them,
 * GCC/Clang may auto-fuse the plain C `a * b + c` accumulation into
 * `fmla`, which would reintroduce the divergence in the tail path.
 *
 * This TU must be compiled with `-ffp-contract=off` (enforced via the
 * `arm64_adm_dwt2_neon_lib` carve-out in meson.build).
 */

/* Belt-and-suspenders: suppress compiler FP contraction at the TU level.
 * Clang honours `#pragma clang fp contract(off)`.  GCC does not implement
 * that pragma but does honour the function attribute below; apply it to
 * every function in this file via the __attribute__ on the definition.
 * The meson.build `-ffp-contract=off` flag is the primary guard; the
 * pragma/attribute are fallbacks for environments that call into this
 * object without the meson carve-out. */
#if defined(__clang__)
#pragma clang fp contract(off)
#endif

#include <arm_neon.h>
#include "float_adm_neon.h"
#include "mem.h"

static const float dwt2_db2_coeffs_lo_s[4] = {0.482962913144690f, 0.836516303737469f,
                                              0.224143868041857f, -0.129409522550921f};

static const float dwt2_db2_coeffs_hi_s[4] = {-0.129409522550921f, -0.224143868041857f,
                                              0.836516303737469f, -0.482962913144690f};

/*
 * float_adm_dwt2_neon_safe — FMA-free NEON implementation of the
 * Daubechies-2 two-band DWT used by float-ADM.
 *
 * This replaces the original float_adm_dwt2_neon which used vmlaq_laneq_f32
 * (a hardware FMA intrinsic).  Each accumulation step is now an explicit
 * vmulq_laneq_f32 + vaddq_f32 pair, matching the scalar reference's
 * separate-mul-then-add rounding behaviour.
 *
 * GCC does not honour `#pragma clang fp contract(off)`, so the scalar
 * tail also carries a per-function attribute as a belt-and-suspenders guard.
 */
#if defined(__GNUC__) && !defined(__clang__)
__attribute__((optimize("-ffp-contract=off")))
#endif
void float_adm_dwt2_neon(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x,
                         int w, int h, int src_stride, int dst_stride)
{
    const float *filter_lo = dwt2_db2_coeffs_lo_s;
    const float *filter_hi = dwt2_db2_coeffs_hi_s;

    int src_px_stride = src_stride / sizeof(float);
    int dst_px_stride = dst_stride / sizeof(float);

    /* aligned_malloc may return NULL on OOM; the scalar reference
     * (adm_tools.c::adm_dwt2_s) treats this as -ENOMEM and aborts the
     * frame.  This function signature is `void` (matched against the
     * AVX-512 / AVX2 siblings — wiring tracked in adm.c ADR-0873
     * follow-up), so we mirror the AVX-512 sibling's behaviour:
     * release whichever allocation succeeded and silently no-op the
     * transform.  Without this guard the NEON stores at the vertical
     * pass NULL-deref and segfault. */
    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    float *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmplo || !tmphi) {
        aligned_free(tmplo);
        aligned_free(tmphi);
        return;
    }

    /* Load filter coefficients into NEON registers. */
    const float32x4_t flo = vld1q_f32(filter_lo);
    const float32x4_t fhi = vld1q_f32(filter_hi);

    int i, j;

    for (i = 0; i < (h + 1) / 2; ++i) {
        const float *row0 = src + ind_y[0][i] * src_px_stride;
        const float *row1 = src + ind_y[1][i] * src_px_stride;
        const float *row2 = src + ind_y[2][i] * src_px_stride;
        const float *row3 = src + ind_y[3][i] * src_px_stride;

        /* Vertical pass: process 4 columns at a time with NEON.
         *
         * Use explicit vmulq_laneq_f32 + vaddq_f32 pairs instead of
         * vmlaq_laneq_f32.  vmlaq_laneq_f32 is a hardware FMA intrinsic
         * (maps to `fmla`) and produces a single-rounded result that
         * diverges from the scalar reference by ~1 ULP.  The explicit
         * two-step form emits separate `fmul` + `fadd` instructions,
         * preserving the same two-rounding sequence used in the scalar
         * path.  ADR-1057. */
        j = 0;
        for (; j + 3 < w; j += 4) {
            float32x4_t s0 = vld1q_f32(row0 + j);
            float32x4_t s1 = vld1q_f32(row1 + j);
            float32x4_t s2 = vld1q_f32(row2 + j);
            float32x4_t s3 = vld1q_f32(row3 + j);

            /* lo = filter_lo[0]*s0 + filter_lo[1]*s1
             *    + filter_lo[2]*s2 + filter_lo[3]*s3
             * Each step: acc = acc + (coeff_lane * sN) — two roundings,
             * matching the scalar accumulation. */
            float32x4_t lo = vmulq_laneq_f32(s0, flo, 0);
            lo = vaddq_f32(lo, vmulq_laneq_f32(s1, flo, 1));
            lo = vaddq_f32(lo, vmulq_laneq_f32(s2, flo, 2));
            lo = vaddq_f32(lo, vmulq_laneq_f32(s3, flo, 3));
            vst1q_f32(tmplo + j, lo);

            /* hi = filter_hi[0]*s0 + filter_hi[1]*s1
             *    + filter_hi[2]*s2 + filter_hi[3]*s3 */
            float32x4_t hi = vmulq_laneq_f32(s0, fhi, 0);
            hi = vaddq_f32(hi, vmulq_laneq_f32(s1, fhi, 1));
            hi = vaddq_f32(hi, vmulq_laneq_f32(s2, fhi, 2));
            hi = vaddq_f32(hi, vmulq_laneq_f32(s3, fhi, 3));
            vst1q_f32(tmphi + j, hi);
        }

        /* Scalar tail for remaining columns.
         * Protected by the per-TU `#pragma clang fp contract(off)` (Clang)
         * and per-function GCC attribute above — prevents auto-fusion of
         * these `a * b` + `+=` pairs into `fmla`. */
        for (; j < w; ++j) {
            float s0 = row0[j];
            float s1 = row1[j];
            float s2 = row2[j];
            float s3 = row3[j];

            tmplo[j] =
                filter_lo[0] * s0 + filter_lo[1] * s1 + filter_lo[2] * s2 + filter_lo[3] * s3;
            tmphi[j] =
                filter_hi[0] * s0 + filter_hi[1] * s1 + filter_hi[2] * s2 + filter_hi[3] * s3;
        }

        /* Horizontal pass (lo and hi): scalar due to indirect indexing. */
        for (j = 0; j < (w + 1) / 2; ++j) {
            int j0 = ind_x[0][j];
            int j1 = ind_x[1][j];
            int j2 = ind_x[2][j];
            int j3 = ind_x[3][j];

            float sl0 = tmplo[j0];
            float sl1 = tmplo[j1];
            float sl2 = tmplo[j2];
            float sl3 = tmplo[j3];

            float accum;
            int off = i * dst_px_stride + j;

            accum = filter_lo[0] * sl0;
            accum += filter_lo[1] * sl1;
            accum += filter_lo[2] * sl2;
            accum += filter_lo[3] * sl3;
            dst->band_a[off] = accum;

            accum = filter_hi[0] * sl0;
            accum += filter_hi[1] * sl1;
            accum += filter_hi[2] * sl2;
            accum += filter_hi[3] * sl3;
            dst->band_v[off] = accum;

            float sh0 = tmphi[j0];
            float sh1 = tmphi[j1];
            float sh2 = tmphi[j2];
            float sh3 = tmphi[j3];

            accum = filter_lo[0] * sh0;
            accum += filter_lo[1] * sh1;
            accum += filter_lo[2] * sh2;
            accum += filter_lo[3] * sh3;
            dst->band_h[off] = accum;

            accum = filter_hi[0] * sh0;
            accum += filter_hi[1] * sh1;
            accum += filter_hi[2] * sh2;
            accum += filter_hi[3] * sh3;
            dst->band_d[off] = accum;
        }
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
}
