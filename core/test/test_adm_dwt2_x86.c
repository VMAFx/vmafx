/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * x86-vs-scalar bit-exactness for the ADM DWT2 kernel.
 *
 * Regression test for x86 half_w_modN tail bound defect (T-UPSTREAM-1564).
 * Tests scalar adm_dwt2_8 against adm_dwt2_8_avx2 and adm_dwt2_8_avx512
 * on widths where (w + 1) / 2 % N == 1, ensuring the last column is correctly
 * handled by the scalar mirror path and not the vector contiguous load path.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "test.h"
#include "cpu.h"

#include "feature/integer_adm.h"

#if ARCH_X86
#include "feature/x86/adm_avx2.h"
#if HAVE_AVX512
#include "feature/x86/adm_avx512.h"
#endif

/* Mirrors dwt2_src_indices_filt() in integer_adm.c, which is static there. */
static void ref_src_indices(int **ind_y, int **ind_x, int w, int h)
{
    const int h_half = (h + 1) / 2;
    const int w_half = (w + 1) / 2;
    int i, j, ind0, ind1, ind2, ind3;

    ind_y[0][0] = 1;
    ind_y[1][0] = 0;
    ind_y[2][0] = 1;
    ind_y[3][0] = 2;
    for (i = 1; i < h_half - 2; ++i) {
        ind1 = 2 * i;
        ind_y[0][i] = ind1 - 1;
        ind_y[1][i] = ind1;
        ind_y[2][i] = ind1 + 1;
        ind_y[3][i] = ind1 + 2;
    }
    for (i = (h_half > 2) ? h_half - 2 : 1; i < h_half; ++i) {
        ind1 = 2 * i;
        ind0 = ind1 - 1;
        ind2 = ind1 + 1;
        ind3 = ind1 + 2;
        if (ind0 >= h)
            ind0 = 2 * h - ind0 - 1;
        if (ind1 >= h)
            ind1 = 2 * h - ind1 - 1;
        if (ind2 >= h)
            ind2 = 2 * h - ind2 - 1;
        if (ind3 >= h)
            ind3 = 2 * h - ind3 - 1;
        ind_y[0][i] = ind0;
        ind_y[1][i] = ind1;
        ind_y[2][i] = ind2;
        ind_y[3][i] = ind3;
    }

    ind_x[0][0] = 1;
    ind_x[1][0] = 0;
    ind_x[2][0] = 1;
    ind_x[3][0] = 2;
    for (j = 1; j < w_half - 2; ++j) {
        ind1 = 2 * j;
        ind_x[0][j] = ind1 - 1;
        ind_x[1][j] = ind1;
        ind_x[2][j] = ind1 + 1;
        ind_x[3][j] = ind1 + 2;
    }
    for (j = (w_half > 2) ? w_half - 2 : 1; j < w_half; ++j) {
        ind1 = 2 * j;
        ind0 = ind1 - 1;
        ind2 = ind1 + 1;
        ind3 = ind1 + 2;
        if (ind0 >= w)
            ind0 = 2 * w - ind0 - 1;
        if (ind1 >= w)
            ind1 = 2 * w - ind1 - 1;
        if (ind2 >= w)
            ind2 = 2 * w - ind2 - 1;
        if (ind3 >= w)
            ind3 = 2 * w - ind3 - 1;
        ind_x[0][j] = ind0;
        ind_x[1][j] = ind1;
        ind_x[2][j] = ind2;
        ind_x[3][j] = ind3;
    }
}

/* Transcribed from adm_dwt2_8() in integer_adm.c (static there). */
static void ref_adm_dwt2_8(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                           int h, int src_stride, int dst_stride)
{
    const int16_t *flo = dwt2_db2_coeffs_lo;
    const int16_t *fhi = dwt2_db2_coeffs_hi;
    const int16_t shift_VP = 8, shift_HP = 16;
    const int32_t add_VP = 128, add_HP = 32768;
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;
    int32_t accum;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        for (int j = 0; j < w; ++j) {
            uint16_t u[4];
            for (int t = 0; t < 4; ++t)
                u[t] = src[ind_y[t][i] * src_stride + j];

            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)flo[t] * (int32_t)u[t];
            accum -= (int32_t)dwt2_db2_coeffs_lo_sum * add_VP;
            tmplo[j] = (int16_t)((accum + add_VP) >> shift_VP);

            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)fhi[t] * (int32_t)u[t];
            accum -= (int32_t)dwt2_db2_coeffs_hi_sum * add_VP;
            tmphi[j] = (int16_t)((accum + add_VP) >> shift_VP);
        }

        for (int j = 0; j < (w + 1) / 2; ++j) {
            const int jx[4] = {ind_x[0][j], ind_x[1][j], ind_x[2][j], ind_x[3][j]};
            int16_t s[4];

            for (int t = 0; t < 4; ++t)
                s[t] = tmplo[jx[t]];
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)flo[t] * s[t];
            dst->band_a[i * dst_stride + j] = (int16_t)((accum + add_HP) >> shift_HP);
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)fhi[t] * s[t];
            dst->band_v[i * dst_stride + j] = (int16_t)((accum + add_HP) >> shift_HP);

            for (int t = 0; t < 4; ++t)
                s[t] = tmphi[jx[t]];
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)flo[t] * s[t];
            dst->band_h[i * dst_stride + j] = (int16_t)((accum + add_HP) >> shift_HP);
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)fhi[t] * s[t];
            dst->band_d[i * dst_stride + j] = (int16_t)((accum + add_HP) >> shift_HP);
        }
    }
}
#endif /* ARCH_X86 */

static char *test_adm_dwt2_8_avx2_matches_scalar(void)
{
#if !ARCH_X86
    return NULL;
#else
    const unsigned flags = vmaf_get_cpu_flags();
    if (!(flags & VMAF_X86_CPU_FLAG_AVX2))
        return NULL;

    const int widths[] = {34, 66, 130, 258, 576};
    const int h = 32;

    for (size_t t = 0; t < sizeof(widths) / sizeof(widths[0]); ++t) {
        const int w = widths[t];
        const int w_half = (w + 1) / 2, h_half = (h + 1) / 2;
        const int dst_stride = w_half;

        uint8_t *src = malloc((size_t)w * h);
        mu_assert("malloc failed for ADM DWT2 source", src);
        int16_t *bands[8];
        AdmBuffer buf = {0};
        int *iy[4], *ix[4];
        adm_dwt_band_t ref_band = {0}, simd_band = {0};
        uint32_t seed = 0x5eed0000u ^ (uint32_t)(w * 131 + h);

        for (int k = 0; k < 4; ++k) {
            iy[k] = calloc((size_t)h_half + 64, sizeof(int));
            mu_assert("calloc failed for ADM DWT2 y indices", iy[k]);
            ix[k] = calloc((size_t)w_half + 64, sizeof(int));
            mu_assert("calloc failed for ADM DWT2 x indices", ix[k]);
            buf.ind_y[k] = iy[k];
            buf.ind_x[k] = ix[k];
        }
        for (int k = 0; k < 8; ++k) {
            bands[k] = calloc((size_t)h_half * dst_stride + 64, sizeof(int16_t));
            mu_assert("calloc failed for ADM DWT2 band", bands[k]);
        }
        buf.tmp_ref = calloc((size_t)w * 8 + 256, sizeof(int16_t));
        mu_assert("calloc failed for ADM DWT2 scratch", buf.tmp_ref);

        ref_band.band_a = bands[0];
        ref_band.band_v = bands[1];
        ref_band.band_h = bands[2];
        ref_band.band_d = bands[3];
        simd_band.band_a = bands[4];
        simd_band.band_v = bands[5];
        simd_band.band_h = bands[6];
        simd_band.band_d = bands[7];

        for (int i = 0; i < w * h; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            src[i] = (uint8_t)(seed & 0xFF);
        }
        ref_src_indices(buf.ind_y, buf.ind_x, w, h);

        ref_adm_dwt2_8(src, &ref_band, &buf, w, h, w, dst_stride);
        memset(buf.tmp_ref, 0, ((size_t)w * 8 + 256) * sizeof(int16_t));
        adm_dwt2_8_avx2(src, &simd_band, &buf, w, h, w, dst_stride);

        const char *names[4] = {"band_a", "band_v", "band_h", "band_d"};
        for (int b = 0; b < 4; ++b) {
            for (int i = 0; i < h_half; ++i) {
                for (int j = 0; j < w_half; ++j) {
                    const int idx = i * dst_stride + j;
                    if (bands[b][idx] != bands[b + 4][idx]) {
                        fprintf(stderr, "  AVX2 %dx%d %s[%d][%d]%s: scalar %d != avx2 %d\n", w, h,
                                names[b], i, j, (j == w_half - 1) ? " (last col)" : "",
                                bands[b][idx], bands[b + 4][idx]);
                        for (int k = 0; k < 4; ++k) {
                            free(iy[k]);
                            free(ix[k]);
                        }
                        for (int k = 0; k < 8; ++k)
                            free(bands[k]);
                        free(buf.tmp_ref);
                        free(src);
                        mu_assert("adm_dwt2_8_avx2 diverges from the scalar reference", 0);
                    }
                }
            }
        }

        for (int k = 0; k < 4; ++k) {
            free(iy[k]);
            free(ix[k]);
        }
        for (int k = 0; k < 8; ++k)
            free(bands[k]);
        free(buf.tmp_ref);
        free(src);
    }
    return NULL;
#endif
}

static char *test_adm_dwt2_8_avx512_matches_scalar(void)
{
#if !(ARCH_X86 && HAVE_AVX512)
    return NULL;
#else
    const unsigned flags = vmaf_get_cpu_flags();
    if (!(flags & VMAF_X86_CPU_FLAG_AVX512))
        return NULL;

    const int widths[] = {34, 66, 130, 258, 576};
    const int h = 32;

    for (size_t t = 0; t < sizeof(widths) / sizeof(widths[0]); ++t) {
        const int w = widths[t];
        const int w_half = (w + 1) / 2, h_half = (h + 1) / 2;
        const int dst_stride = w_half;

        uint8_t *src = malloc((size_t)w * h);
        mu_assert("malloc failed for ADM DWT2 source", src);
        int16_t *bands[8];
        AdmBuffer buf = {0};
        int *iy[4], *ix[4];
        adm_dwt_band_t ref_band = {0}, simd_band = {0};
        uint32_t seed = 0x5eed0000u ^ (uint32_t)(w * 131 + h);

        for (int k = 0; k < 4; ++k) {
            iy[k] = calloc((size_t)h_half + 64, sizeof(int));
            mu_assert("calloc failed for ADM DWT2 y indices", iy[k]);
            ix[k] = calloc((size_t)w_half + 64, sizeof(int));
            mu_assert("calloc failed for ADM DWT2 x indices", ix[k]);
            buf.ind_y[k] = iy[k];
            buf.ind_x[k] = ix[k];
        }
        for (int k = 0; k < 8; ++k) {
            bands[k] = calloc((size_t)h_half * dst_stride + 64, sizeof(int16_t));
            mu_assert("calloc failed for ADM DWT2 band", bands[k]);
        }
        buf.tmp_ref = calloc((size_t)w * 8 + 256, sizeof(int16_t));
        mu_assert("calloc failed for ADM DWT2 scratch", buf.tmp_ref);

        ref_band.band_a = bands[0];
        ref_band.band_v = bands[1];
        ref_band.band_h = bands[2];
        ref_band.band_d = bands[3];
        simd_band.band_a = bands[4];
        simd_band.band_v = bands[5];
        simd_band.band_h = bands[6];
        simd_band.band_d = bands[7];

        for (int i = 0; i < w * h; ++i) {
            seed ^= seed << 13;
            seed ^= seed >> 17;
            seed ^= seed << 5;
            src[i] = (uint8_t)(seed & 0xFF);
        }
        ref_src_indices(buf.ind_y, buf.ind_x, w, h);

        ref_adm_dwt2_8(src, &ref_band, &buf, w, h, w, dst_stride);
        memset(buf.tmp_ref, 0, ((size_t)w * 8 + 256) * sizeof(int16_t));
        adm_dwt2_8_avx512(src, &simd_band, &buf, w, h, w, dst_stride);

        const char *names[4] = {"band_a", "band_v", "band_h", "band_d"};
        for (int b = 0; b < 4; ++b) {
            for (int i = 0; i < h_half; ++i) {
                for (int j = 0; j < w_half; ++j) {
                    const int idx = i * dst_stride + j;
                    if (bands[b][idx] != bands[b + 4][idx]) {
                        fprintf(stderr, "  AVX512 %dx%d %s[%d][%d]%s: scalar %d != avx512 %d\n", w,
                                h, names[b], i, j, (j == w_half - 1) ? " (last col)" : "",
                                bands[b][idx], bands[b + 4][idx]);
                        for (int k = 0; k < 4; ++k) {
                            free(iy[k]);
                            free(ix[k]);
                        }
                        for (int k = 0; k < 8; ++k)
                            free(bands[k]);
                        free(buf.tmp_ref);
                        free(src);
                        mu_assert("adm_dwt2_8_avx512 diverges from the scalar reference", 0);
                    }
                }
            }
        }

        for (int k = 0; k < 4; ++k) {
            free(iy[k]);
            free(ix[k]);
        }
        for (int k = 0; k < 8; ++k)
            free(bands[k]);
        free(buf.tmp_ref);
        free(src);
    }
    return NULL;
#endif
}

char *run_tests(void)
{
#if ARCH_X86
    vmaf_init_cpu();
    mu_run_test(test_adm_dwt2_8_avx2_matches_scalar);
#if HAVE_AVX512
    mu_run_test(test_adm_dwt2_8_avx512_matches_scalar);
#endif
#else
    (void)fprintf(stderr, "skipping: non-x86 arch\n");
    (void)test_adm_dwt2_8_avx2_matches_scalar;
#endif
    return NULL;
}
