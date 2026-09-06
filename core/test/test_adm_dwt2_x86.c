/**
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * x86-vs-scalar bit-exactness for the ADM DWT2 kernel.
 *
 * Regression test for the x86 half_w_modN tail bound defect (T-UPSTREAM-1564).
 * Runs the scalar adm_dwt2_8 reference against adm_dwt2_8_avx2 and
 * adm_dwt2_8_avx512 on widths where (w + 1) / 2 % N == 1, where the vector
 * loop used to own the last column instead of leaving it to the scalar mirror
 * path. w = 576 is the control (no such remainder).
 *
 * All working buffers are file-scope arrays sized for the largest case: the
 * kernels only use unaligned loads and stores, and static storage keeps the
 * test free of allocation-failure paths (NASA/JPL Power of 10 rule 3).
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
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

#define ADM_DWT2_MAX_W 576
#define ADM_DWT2_H 32
#define ADM_DWT2_H_HALF ((ADM_DWT2_H + 1) / 2)
#define ADM_DWT2_MAX_W_HALF ((ADM_DWT2_MAX_W + 1) / 2)
#define ADM_DWT2_BAND_ELEMS (ADM_DWT2_H_HALF * ADM_DWT2_MAX_W_HALF + 64)
#define ADM_DWT2_TMP_ELEMS (ADM_DWT2_MAX_W * 8 + 256)

typedef void (*adm_dwt2_8_fn)(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                              int h, int src_stride, int dst_stride);

/* g_bands[0..3] hold the scalar reference bands, g_bands[4..7] the SIMD ones,
 * in adm_dwt_band_t order: a, v, h, d. */
static uint8_t g_src[ADM_DWT2_MAX_W * ADM_DWT2_H];
static int16_t g_bands[8][ADM_DWT2_BAND_ELEMS];
static int16_t g_tmp[ADM_DWT2_TMP_ELEMS];
static int g_ind_y[4][ADM_DWT2_H_HALF + 64];
static int g_ind_x[4][ADM_DWT2_MAX_W_HALF + 64];

static const int g_widths[] = {34, 66, 130, 258, 576};

/* One axis of dwt2_src_indices_filt() in integer_adm.c, which is static there.
 * The vertical and horizontal index tables differ only in the extent, so both
 * are built by this helper. */
static void ref_src_indices_axis(int **ind, int n)
{
    const int n_half = (n + 1) / 2;

    ind[0][0] = 1;
    ind[1][0] = 0;
    ind[2][0] = 1;
    ind[3][0] = 2;
    for (int i = 1; i < n_half - 2; ++i) {
        const int centre = 2 * i;
        ind[0][i] = centre - 1;
        ind[1][i] = centre;
        ind[2][i] = centre + 1;
        ind[3][i] = centre + 2;
    }
    for (int i = (n_half > 2) ? n_half - 2 : 1; i < n_half; ++i) {
        int idx[4] = {2 * i - 1, 2 * i, 2 * i + 1, 2 * i + 2};
        for (int t = 0; t < 4; ++t) {
            if (idx[t] >= n)
                idx[t] = 2 * n - idx[t] - 1;
            ind[t][i] = idx[t];
        }
    }
}

/* Vertical pass of adm_dwt2_8() in integer_adm.c (static there): filters the
 * four mirrored source rows of output row i into the tmplo / tmphi scratch. */
static void ref_dwt2_8_vertical(const uint8_t *src, int **ind_y, int16_t *tmplo, int16_t *tmphi,
                                int i, int w, int src_stride)
{
    const int16_t shift_VP = 8;
    const int32_t add_VP = 128;

    for (int j = 0; j < w; ++j) {
        uint16_t u[4];
        int32_t accum_lo = 0;
        int32_t accum_hi = 0;

        for (int t = 0; t < 4; ++t)
            u[t] = src[(ptrdiff_t)ind_y[t][i] * src_stride + j];
        for (int t = 0; t < 4; ++t) {
            accum_lo += (int32_t)dwt2_db2_coeffs_lo[t] * (int32_t)u[t];
            accum_hi += (int32_t)dwt2_db2_coeffs_hi[t] * (int32_t)u[t];
        }
        accum_lo -= (int32_t)dwt2_db2_coeffs_lo_sum * add_VP;
        accum_hi -= (int32_t)dwt2_db2_coeffs_hi_sum * add_VP;
        tmplo[j] = (int16_t)((accum_lo + add_VP) >> shift_VP);
        tmphi[j] = (int16_t)((accum_hi + add_VP) >> shift_VP);
    }
}

/* Horizontal pass of adm_dwt2_8(): the mirrored ind_x taps applied to the
 * scratch rows. This is the pass the tail-bound defect corrupted. */
static void ref_dwt2_8_horizontal(const adm_dwt_band_t *dst, int **ind_x, const int16_t *tmplo,
                                  const int16_t *tmphi, int i, int w, int dst_stride)
{
    const int16_t shift_HP = 16;
    const int32_t add_HP = 32768;

    for (int j = 0; j < (w + 1) / 2; ++j) {
        const ptrdiff_t out = (ptrdiff_t)i * dst_stride + j;
        int32_t accum[4] = {0, 0, 0, 0};

        for (int t = 0; t < 4; ++t) {
            const int16_t lo = tmplo[ind_x[t][j]];
            const int16_t hi = tmphi[ind_x[t][j]];
            accum[0] += (int32_t)dwt2_db2_coeffs_lo[t] * lo;
            accum[1] += (int32_t)dwt2_db2_coeffs_hi[t] * lo;
            accum[2] += (int32_t)dwt2_db2_coeffs_lo[t] * hi;
            accum[3] += (int32_t)dwt2_db2_coeffs_hi[t] * hi;
        }
        dst->band_a[out] = (int16_t)((accum[0] + add_HP) >> shift_HP);
        dst->band_v[out] = (int16_t)((accum[1] + add_HP) >> shift_HP);
        dst->band_h[out] = (int16_t)((accum[2] + add_HP) >> shift_HP);
        dst->band_d[out] = (int16_t)((accum[3] + add_HP) >> shift_HP);
    }
}

/* Transcribed from adm_dwt2_8() in integer_adm.c (static there). */
static void ref_adm_dwt2_8(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                           int h, int src_stride, int dst_stride)
{
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        ref_dwt2_8_vertical(src, buf->ind_y, tmplo, tmphi, i, w, src_stride);
        ref_dwt2_8_horizontal(dst, buf->ind_x, tmplo, tmphi, i, w, dst_stride);
    }
}

/* xorshift32 — deterministic per (w, h) so a failure is always reproducible. */
static void fill_source(int w, int h)
{
    uint32_t seed = 0x5eed0000u ^ (uint32_t)(w * 131 + h);

    for (int i = 0; i < w * h; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        g_src[i] = (uint8_t)(seed & 0xFFu);
    }
}

/* Returns 1 when every sample of every band matches, 0 after reporting the
 * first divergence. */
static int bands_match(const char *label, int w, int h, int dst_stride)
{
    static const char *const names[4] = {"band_a", "band_v", "band_h", "band_d"};
    const int w_half = (w + 1) / 2;
    const int h_half = (h + 1) / 2;

    for (int b = 0; b < 4; ++b) {
        for (int i = 0; i < h_half; ++i) {
            for (int j = 0; j < w_half; ++j) {
                const int idx = i * dst_stride + j;
                if (g_bands[b][idx] == g_bands[b + 4][idx])
                    continue;
                (void)fprintf(stderr, "  %s %dx%d %s[%d][%d]%s: scalar %d != simd %d\n", label, w,
                              h, names[b], i, j, (j == w_half - 1) ? " (last col)" : "",
                              g_bands[b][idx], g_bands[b + 4][idx]);
                return 0;
            }
        }
    }
    return 1;
}

/* Runs the scalar reference and `kernel` over the same source and compares
 * all four subbands. Returns 1 on bit-exact agreement. */
static int dwt2_case_matches(adm_dwt2_8_fn kernel, const char *label, int w)
{
    const int h = ADM_DWT2_H;
    const int dst_stride = (w + 1) / 2;
    adm_dwt_band_t ref_band = {};
    adm_dwt_band_t simd_band = {};
    AdmBuffer buf = {0};

    for (int k = 0; k < 4; ++k) {
        buf.ind_y[k] = g_ind_y[k];
        buf.ind_x[k] = g_ind_x[k];
    }
    buf.tmp_ref = g_tmp;
    ref_band.band_a = g_bands[0];
    ref_band.band_v = g_bands[1];
    ref_band.band_h = g_bands[2];
    ref_band.band_d = g_bands[3];
    simd_band.band_a = g_bands[4];
    simd_band.band_v = g_bands[5];
    simd_band.band_h = g_bands[6];
    simd_band.band_d = g_bands[7];

    memset(g_bands, 0, sizeof(g_bands));
    memset(g_ind_y, 0, sizeof(g_ind_y));
    memset(g_ind_x, 0, sizeof(g_ind_x));
    fill_source(w, h);
    ref_src_indices_axis(buf.ind_y, h);
    ref_src_indices_axis(buf.ind_x, w);

    memset(g_tmp, 0, sizeof(g_tmp));
    ref_adm_dwt2_8(g_src, &ref_band, &buf, w, h, w, dst_stride);
    memset(g_tmp, 0, sizeof(g_tmp));
    kernel(g_src, &simd_band, &buf, w, h, w, dst_stride);

    return bands_match(label, w, h, dst_stride);
}
#endif /* ARCH_X86 */

static char *test_adm_dwt2_8_avx2_matches_scalar(void)
{
#if !ARCH_X86
    return nullptr;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2))
        return nullptr;

    for (size_t t = 0; t < sizeof(g_widths) / sizeof(g_widths[0]); ++t) {
        mu_assert("adm_dwt2_8_avx2 diverges from the scalar reference",
                  dwt2_case_matches(adm_dwt2_8_avx2, "AVX2", g_widths[t]));
    }
    return nullptr;
#endif
}

static char *test_adm_dwt2_8_avx512_matches_scalar(void)
{
#if !(ARCH_X86 && HAVE_AVX512)
    return nullptr;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512))
        return nullptr;

    for (size_t t = 0; t < sizeof(g_widths) / sizeof(g_widths[0]); ++t) {
        mu_assert("adm_dwt2_8_avx512 diverges from the scalar reference",
                  dwt2_case_matches(adm_dwt2_8_avx512, "AVX512", g_widths[t]));
    }
    return nullptr;
#endif
}

char *run_tests(void)
{
#if ARCH_X86
    vmaf_init_cpu();
    mu_run_test(test_adm_dwt2_8_avx2_matches_scalar);
#if HAVE_AVX512
    mu_run_test(test_adm_dwt2_8_avx512_matches_scalar);
#else
    (void)test_adm_dwt2_8_avx512_matches_scalar;
#endif
#else
    (void)fprintf(stderr, "skipping: non-x86 arch\n");
    (void)test_adm_dwt2_8_avx2_matches_scalar;
    (void)test_adm_dwt2_8_avx512_matches_scalar;
#endif
    return nullptr;
}
