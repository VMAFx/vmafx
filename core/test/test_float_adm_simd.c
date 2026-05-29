/**
 *
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
 * Bit-exactness test for the float-ADM AVX2/NEON SIMD kernels (PR #116 F1).
 *
 * Verifies that the four dispatched kernels produce results within the
 * ADR-0418 tolerance contract:
 *
 *   float_adm_dwt2_{avx2,neon}    — bit-exact to adm_dwt2_s (same float
 *                                   arithmetic, same reduction order).
 *   float_adm_csf_{avx2,neon}     — bit-exact per element (element-wise
 *                                   multiply; no reduction).
 *   float_adm_csf_den_scale_{avx2,neon} — accumulates in double (ADR-0418);
 *                                   match scalar within 1e-5 relative tol.
 *   float_adm_sum_cube_{avx2,neon}      — idem.
 *
 * Note on reduction-order mismatch (F2 pre-condition):
 *   float_adm_csf_den_scale and float_adm_sum_cube use a tree reduction
 *   (AVX2: 8-wide horizontal) whereas the scalar reference uses a
 *   left-to-right sequential double accumulation.  Per ADR-0418 both
 *   paths use double accumulators so the final powf(accum, 1/p) output
 *   is within 1e-5 relative tolerance even for large images.  This file
 *   tests within that tolerance budget.
 *
 * Boilerplate provided by `simd_bitexact_test.h` (ADR-0245).
 */

#include <math.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
/* clang-format off — test.h has no header guard; must precede harness. */
#include "simd_bitexact_test.h"
/* clang-format on */

#include "mem.h"
#include "feature/adm_tools.h"

#if ARCH_X86
#include "feature/x86/float_adm_avx2.h"
#endif
#if ARCH_AARCH64
#include "feature/arm64/float_adm_neon.h"
#endif

/* -----------------------------------------------------------------------
 * Test dimensions: non-power-of-two so tails are exercised.
 * w=37 gives an 8-wide-AVX2 body of 4 chunks + 5-element scalar tail.
 * height 5 rows, one border row on each side (border_factor=0.1).
 * --------------------------------------------------------------------- */
#define FADM_W 37
#define FADM_H 20
/* stride aligned to 32 bytes (8 floats) */
#define FADM_STRIDE_PX ((FADM_W + 7) & ~7)
#define FADM_STRIDE_B ((size_t)FADM_STRIDE_PX * sizeof(float))
#define FADM_NBANDS 4

static float *alloc_band(void)
{
    return (float *)simd_test_aligned_malloc(FADM_STRIDE_B * FADM_H, 32);
}

static void fill_band(float *band, uint32_t *state)
{
    for (int i = 0; i < FADM_STRIDE_PX * FADM_H; ++i) {
        uint32_t r = simd_test_xorshift32(state);
        /* Values in [-4.0, 4.0] — representative of DWT coefficients. */
        band[i] = ((float)(int)(r & 0xFF) - 128.0f) / 32.0f;
    }
}

/* -----------------------------------------------------------------------
 * 1. float_adm_csf: per-element multiply + abs/30.  Bit-exact.
 * --------------------------------------------------------------------- */

static char *test_float_adm_csf_bitexact(void)
{
#if !ARCH_X86 && !ARCH_AARCH64
    (void)fprintf(stderr, "  skip: not x86 or aarch64\n");
    return NULL;
#else
    uint32_t state = 0xdeadbeef;
    float *src = alloc_band();
    float *dst_scalar = alloc_band();
    float *flt_scalar = alloc_band();
    float *dst_simd = alloc_band();
    float *flt_simd = alloc_band();

    if (!src || !dst_scalar || !flt_scalar || !dst_simd || !flt_simd) {
        simd_test_aligned_free(src);
        simd_test_aligned_free(dst_scalar);
        simd_test_aligned_free(flt_scalar);
        simd_test_aligned_free(dst_simd);
        simd_test_aligned_free(flt_simd);
        return "alloc failed";
    }

    fill_band(src, &state);

    const float factor = 0.7353553f;
    const float one_by_30 = 0.0333333351f;

    /* Scalar reference: adm_csf_band_scalar logic (same as FLOAT_ONE_BY_30). */
    for (int i = 0; i < FADM_H; ++i) {
        for (int j = 0; j < FADM_W; ++j) {
            float dv = factor * src[i * FADM_STRIDE_PX + j];
            dst_scalar[i * FADM_STRIDE_PX + j] = dv;
            flt_scalar[i * FADM_STRIDE_PX + j] = one_by_30 * fabsf(dv);
        }
    }

#if ARCH_X86
    if (!simd_test_have_avx2()) {
        (void)fprintf(stderr, "  skip: no AVX2\n");
        goto cleanup_csf;
    }
    float_adm_csf_avx2(src, dst_simd, flt_simd, FADM_W, FADM_H, (int)FADM_STRIDE_B,
                       (int)FADM_STRIDE_B, factor, one_by_30);
#elif ARCH_AARCH64
    float_adm_csf_neon(src, dst_simd, flt_simd, FADM_W, FADM_H, (int)FADM_STRIDE_B,
                       (int)FADM_STRIDE_B, factor, one_by_30);
#endif

    /* dst and flt must be bit-exact (element-wise multiply, no reduction). */
    for (int i = 0; i < FADM_H; ++i) {
        for (int j = 0; j < FADM_W; ++j) {
            float ds = dst_scalar[i * FADM_STRIDE_PX + j];
            float dv = dst_simd[i * FADM_STRIDE_PX + j];
            if (ds != dv) {
                (void)fprintf(stderr, "  csf dst[%d][%d]: scalar=%a simd=%a\n", i, j, (double)ds,
                              (double)dv);
                simd_test_aligned_free(src);
                simd_test_aligned_free(dst_scalar);
                simd_test_aligned_free(flt_scalar);
                simd_test_aligned_free(dst_simd);
                simd_test_aligned_free(flt_simd);
                return "float_adm_csf dst not bit-exact";
            }
            float fs = flt_scalar[i * FADM_STRIDE_PX + j];
            float fv = flt_simd[i * FADM_STRIDE_PX + j];
            if (fs != fv) {
                (void)fprintf(stderr, "  csf flt[%d][%d]: scalar=%a simd=%a\n", i, j, (double)fs,
                              (double)fv);
                simd_test_aligned_free(src);
                simd_test_aligned_free(dst_scalar);
                simd_test_aligned_free(flt_scalar);
                simd_test_aligned_free(dst_simd);
                simd_test_aligned_free(flt_simd);
                return "float_adm_csf flt not bit-exact";
            }
        }
    }

cleanup_csf:
    simd_test_aligned_free(src);
    simd_test_aligned_free(dst_scalar);
    simd_test_aligned_free(flt_scalar);
    simd_test_aligned_free(dst_simd);
    simd_test_aligned_free(flt_simd);
    return NULL;
#endif /* ARCH_X86 || ARCH_AARCH64 */
}

/* -----------------------------------------------------------------------
 * 2. float_adm_sum_cube: reduction within ADR-0418 double tolerance.
 * --------------------------------------------------------------------- */

static char *test_float_adm_sum_cube_tolerance(void)
{
#if !ARCH_X86 && !ARCH_AARCH64
    (void)fprintf(stderr, "  skip: not x86 or aarch64\n");
    return NULL;
#else
    uint32_t state = 0xcafebabe;
    float *band = alloc_band();
    if (!band) {
        return "alloc failed";
    }
    fill_band(band, &state);

    const int left = (int)(FADM_W * 0.1 - 0.5);
    const int top = (int)(FADM_H * 0.1 - 0.5);
    const int right = FADM_W - left;
    const int bottom = FADM_H - top;

    /* Scalar reference: sequential double accumulation (ADR-0418 scalar). */
    double accum_scalar = 0.0;
    for (int i = top; i < bottom; ++i) {
        double row = 0.0;
        for (int j = left; j < right; ++j) {
            float v = fabsf(band[i * FADM_STRIDE_PX + j]);
            row += (double)(v * v * v);
        }
        accum_scalar += row;
    }

    float result_scalar = (float)accum_scalar;
    float result_simd = 0.0f;

#if ARCH_X86
    if (!simd_test_have_avx2()) {
        (void)fprintf(stderr, "  skip: no AVX2\n");
        simd_test_aligned_free(band);
        return NULL;
    }
    result_simd =
        float_adm_sum_cube_avx2(band, FADM_W, FADM_H, (int)FADM_STRIDE_B, left, top, right, bottom);
#elif ARCH_AARCH64
    result_simd =
        float_adm_sum_cube_neon(band, FADM_W, FADM_H, (int)FADM_STRIDE_B, left, top, right, bottom);
#endif

    simd_test_aligned_free(band);

    /* ADR-0418 tolerance: both paths use double accumulators; tree vs
     * sequential order gives at most 1e-5 relative difference. */
    const double tol = 1e-5;
    double rel = (result_scalar != 0.0f) ?
                     fabs((double)(result_simd - result_scalar)) / fabs((double)result_scalar) :
                     fabs((double)result_simd);
    if (rel > tol) {
        (void)fprintf(stderr, "  sum_cube: scalar=%a simd=%a rel=%g\n", (double)result_scalar,
                      (double)result_simd, rel);
        return "float_adm_sum_cube exceeds ADR-0418 double tolerance";
    }
    return NULL;
#endif /* ARCH_X86 || ARCH_AARCH64 */
}

/* -----------------------------------------------------------------------
 * 3. float_adm_csf_den_scale: reduction within ADR-0418 double tolerance.
 * --------------------------------------------------------------------- */

static char *test_float_adm_csf_den_scale_tolerance(void)
{
#if !ARCH_X86 && !ARCH_AARCH64
    (void)fprintf(stderr, "  skip: not x86 or aarch64\n");
    return NULL;
#else
    uint32_t state = 0x12345678;
    float *band = alloc_band();
    if (!band) {
        return "alloc failed";
    }
    fill_band(band, &state);

    const float factor = 0.7353553f;
    const int left = (int)(FADM_W * 0.1 - 0.5);
    const int top = (int)(FADM_H * 0.1 - 0.5);
    const int right = FADM_W - left;
    const int bottom = FADM_H - top;

    /* Scalar reference. */
    double accum_scalar = 0.0;
    for (int i = top; i < bottom; ++i) {
        double row = 0.0;
        for (int j = left; j < right; ++j) {
            float v = fabsf(factor * band[i * FADM_STRIDE_PX + j]);
            row += (double)(v * v * v);
        }
        accum_scalar += row;
    }
    float result_scalar = (float)accum_scalar;
    float result_simd = 0.0f;

#if ARCH_X86
    if (!simd_test_have_avx2()) {
        (void)fprintf(stderr, "  skip: no AVX2\n");
        simd_test_aligned_free(band);
        return NULL;
    }
    result_simd = float_adm_csf_den_scale_avx2(band, FADM_W, FADM_H, (int)FADM_STRIDE_B, left, top,
                                               right, bottom, factor);
#elif ARCH_AARCH64
    result_simd = float_adm_csf_den_scale_neon(band, FADM_W, FADM_H, (int)FADM_STRIDE_B, left, top,
                                               right, bottom, factor);
#endif

    simd_test_aligned_free(band);

    const double tol = 1e-5;
    double rel = (result_scalar != 0.0f) ?
                     fabs((double)(result_simd - result_scalar)) / fabs((double)result_scalar) :
                     fabs((double)result_simd);
    if (rel > tol) {
        (void)fprintf(stderr, "  csf_den_scale: scalar=%a simd=%a rel=%g\n", (double)result_scalar,
                      (double)result_simd, rel);
        return "float_adm_csf_den_scale exceeds ADR-0418 double tolerance";
    }
    return NULL;
#endif /* ARCH_X86 || ARCH_AARCH64 */
}

/* -----------------------------------------------------------------------
 * 4. float_adm_dwt2: bit-exact to adm_dwt2_s (same float arithmetic).
 * --------------------------------------------------------------------- */

static char *test_float_adm_dwt2_bitexact(void)
{
#if !ARCH_X86 && !ARCH_AARCH64
    (void)fprintf(stderr, "  skip: not x86 or aarch64\n");
    return NULL;
#else
    uint32_t state = 0xfeedface;
    const int w = FADM_W;
    const int h = FADM_H;
    const int src_stride = (int)FADM_STRIDE_B;
    const int dst_stride = (int)(((w / 2 + 1 + 7) & ~7) * sizeof(float));
    const int dst_px_w = dst_stride / (int)sizeof(float);
    const int dst_h = (h + 1) / 2;

    float *src = alloc_band();
    if (!src) {
        return "alloc failed";
    }
    fill_band(src, &state);

    /* Build ind_y and ind_x the same way dwt2_src_indices_filt_s does. */
    int ind_y_buf[4][(FADM_H / 2) + 2];
    int ind_x_buf[4][(FADM_W / 2) + 2];
    int *ind_y[4] = {ind_y_buf[0], ind_y_buf[1], ind_y_buf[2], ind_y_buf[3]};
    int *ind_x[4] = {ind_x_buf[0], ind_x_buf[1], ind_x_buf[2], ind_x_buf[3]};
    dwt2_src_indices_filt_s(ind_y, ind_x, w, h);

    /* Allocate scalar destination bands.  Use calloc-equivalent (zero init)
     * so stride padding does not cause false divergence. */
    size_t band_bytes = (size_t)dst_stride * dst_h;
    float *s_a = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *s_h = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *s_v = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *s_d = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *m_a = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *m_h = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *m_v = (float *)simd_test_aligned_malloc(band_bytes, 32);
    float *m_d = (float *)simd_test_aligned_malloc(band_bytes, 32);

    if (!s_a || !s_h || !s_v || !s_d || !m_a || !m_h || !m_v || !m_d) {
        simd_test_aligned_free(src);
        simd_test_aligned_free(s_a);
        simd_test_aligned_free(s_h);
        simd_test_aligned_free(s_v);
        simd_test_aligned_free(s_d);
        simd_test_aligned_free(m_a);
        simd_test_aligned_free(m_h);
        simd_test_aligned_free(m_v);
        simd_test_aligned_free(m_d);
        return "alloc failed";
    }

    /* Zero-initialise so stride padding doesn't cause false divergence. */
    (void)memset(s_a, 0, band_bytes);
    (void)memset(s_h, 0, band_bytes);
    (void)memset(s_v, 0, band_bytes);
    (void)memset(s_d, 0, band_bytes);
    (void)memset(m_a, 0, band_bytes);
    (void)memset(m_h, 0, band_bytes);
    (void)memset(m_v, 0, band_bytes);
    (void)memset(m_d, 0, band_bytes);

    adm_dwt_band_t_s scalar_dst = {s_a, s_v, s_h, s_d};
    adm_dwt_band_t_s simd_dst = {m_a, m_v, m_h, m_d};

    /* Run scalar. */
    (void)adm_dwt2_s(src, &scalar_dst, ind_y, ind_x, w, h, src_stride, dst_stride);

#if ARCH_X86
    if (!simd_test_have_avx2()) {
        (void)fprintf(stderr, "  skip: no AVX2\n");
        goto cleanup_dwt2;
    }
    float_adm_dwt2_avx2(src, &simd_dst, ind_y, ind_x, w, h, src_stride, dst_stride);
#elif ARCH_AARCH64
    float_adm_dwt2_neon(src, &simd_dst, ind_y, ind_x, w, h, src_stride, dst_stride);
#endif

    /* Compare all four output bands element-by-element.
     * Only compare valid output elements: (w+1)/2 cols x (h+1)/2 rows.
     * Stride padding is uninitialised and must not be compared. */
    const int dst_valid_w = (w + 1) / 2;
    const float *scalar_bands[4] = {s_a, s_h, s_v, s_d};
    const float *simd_bands[4] = {m_a, m_h, m_v, m_d};
    const char *band_names[4] = {"band_a", "band_h", "band_v", "band_d"};

    for (int b = 0; b < FADM_NBANDS; ++b) {
        for (int i = 0; i < dst_h; ++i) {
            for (int j = 0; j < dst_valid_w; ++j) {
                float sv = scalar_bands[b][i * dst_px_w + j];
                float mv = simd_bands[b][i * dst_px_w + j];
                if (sv != mv) {
                    (void)fprintf(stderr, "  dwt2 %s[%d][%d]: scalar=%a simd=%a\n", band_names[b],
                                  i, j, (double)sv, (double)mv);
                    simd_test_aligned_free(src);
                    simd_test_aligned_free(s_a);
                    simd_test_aligned_free(s_h);
                    simd_test_aligned_free(s_v);
                    simd_test_aligned_free(s_d);
                    simd_test_aligned_free(m_a);
                    simd_test_aligned_free(m_h);
                    simd_test_aligned_free(m_v);
                    simd_test_aligned_free(m_d);
                    return "float_adm_dwt2 not bit-exact";
                }
            }
        }
    }

#if ARCH_X86
cleanup_dwt2:
#endif
    simd_test_aligned_free(src);
    simd_test_aligned_free(s_a);
    simd_test_aligned_free(s_h);
    simd_test_aligned_free(s_v);
    simd_test_aligned_free(s_d);
    simd_test_aligned_free(m_a);
    simd_test_aligned_free(m_h);
    simd_test_aligned_free(m_v);
    simd_test_aligned_free(m_d);
    return NULL;
#endif /* ARCH_X86 || ARCH_AARCH64 */
}

char *run_tests(void)
{
    mu_run_test(test_float_adm_csf_bitexact);
    mu_run_test(test_float_adm_sum_cube_tolerance);
    mu_run_test(test_float_adm_csf_den_scale_tolerance);
    mu_run_test(test_float_adm_dwt2_bitexact);
    return NULL;
}
