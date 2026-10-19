/**
 * Copyright 2016-2020 Netflix, Inc.
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * NEON-vs-scalar bit-exactness for the ADM DWT2 kernel.
 *
 * ADR-1057: `adm_dwt2_8_neon`'s `j == 0` horizontal special case summed only
 * three of the four Daubechies-2 taps, dropping `ind_x[3][0]`. The resulting
 * drift was small enough to survive every existing test and only surfaced as a
 * Netflix golden mismatch on ARM (`akiyo 88.030322 != 88.030463`). No unit
 * test covered this kernel on any architecture -- the SIMD suite reaches
 * `adm_cm`, not `adm_dwt2`.
 *
 * This test runs the NEON kernel against a scalar reference transcribed from
 * `adm_dwt2_8` in integer_adm.c and requires bit-exact output across all four
 * subbands, on every width the dispatcher routes to NEON (`w % 8 == 0`) from
 * 16 to 128 and on heights that put the mirrored last row at every phase.
 *
 * Netflix/vmaf ea012e387: the 8-wide horizontal loop had no tail and stored
 * one sample past the half-resolution row on every width; on the last row that
 * sample landed on element [0][0] of the next band in the shared ADM slab.
 * The bands therefore use the stride integer_adm.c derives and start out
 * filled with the simd_bitexact_test.h guard pattern: any store outside the
 * band, including into the slack after its last row, fails the test.
 *
 * Netflix/vmaf b41d2340a: adm_dwt2_s123_combined_neon(), the 32-bit DWT of
 * scales 1 to 3, against the scalar kernels of integer_adm_kernels.h
 * (i4_dwt2_vpass() / i4_dwt2_hpass(), the body of adm_dwt2_s123_combined()),
 * for both pictures, every scale, full-range int32 sources and widths 3 to
 * 70, with the same guard band.
 */

#include <stdbool.h>
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

#include "feature/integer_adm.h"
#include "feature/integer_adm_kernels.h"

#if ARCH_AARCH64
#include "feature/arm64/adm_neon.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this file mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

/* One axis of dwt2_src_indices_filt() in integer_adm.c, which is static there.
 * `len` is the source extent, `len_half` the subsampled extent. */
static void ref_mirror_indices(int **ind, int len, int len_half)
{
    ind[0][0] = 1;
    ind[1][0] = 0;
    ind[2][0] = 1;
    ind[3][0] = 2;

    for (int i = 1; i < len_half - 2; ++i) {
        const int ind1 = 2 * i;
        ind[0][i] = ind1 - 1;
        ind[1][i] = ind1;
        ind[2][i] = ind1 + 1;
        ind[3][i] = ind1 + 2;
    }

    for (int i = (len_half > 2) ? len_half - 2 : 1; i < len_half; ++i) {
        int idx[4] = {(2 * i) - 1, 2 * i, (2 * i) + 1, (2 * i) + 2};
        for (int t = 0; t < 4; ++t) {
            if (idx[t] >= len)
                idx[t] = (2 * len) - idx[t] - 1;
            ind[t][i] = idx[t];
        }
    }
}

/* Mirrors dwt2_src_indices_filt() in integer_adm.c, which is static there. */
static void ref_src_indices(int **ind_y, int **ind_x, int w, int h)
{
    ref_mirror_indices(ind_y, h, (h + 1) / 2);
    ref_mirror_indices(ind_x, w, (w + 1) / 2);
}

/* Transcribed from adm_dwt2_8() in integer_adm.c (static there). */
static void ref_adm_dwt2_8(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                           int h, int src_stride, int dst_stride)
{
    const int16_t *flo = dwt2_db2_coeffs_lo;
    const int16_t *fhi = dwt2_db2_coeffs_hi;
    const int16_t shift_VP = 8;
    const int16_t shift_HP = 16;
    const int32_t add_VP = 128;
    const int32_t add_HP = 32768;
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
/* Trailing elements past each band that no kernel may write. */
#define DWT2_SLACK 64

/* Everything one (w, h) comparison needs. bands[0..3] receive the scalar
 * reference, bands[4..7] the NEON kernel, so band b and band b + 4 hold the
 * two implementations of the same band. */
typedef struct Dwt2Fixture {
    int w;
    int h;
    int w_half;
    int h_half;
    int dst_stride;
    size_t band_elems;
    size_t tmp_elems;
    uint8_t *src;
    int16_t *bands[8];
    int *iy[4];
    int *ix[4];
    AdmBuffer buf;
    adm_dwt_band_t ref_band;
    adm_dwt_band_t simd_band;
} Dwt2Fixture;

static void fixture_free(Dwt2Fixture *f)
{
    for (int k = 0; k < 4; ++k) {
        free(f->iy[k]);
        free(f->ix[k]);
        f->iy[k] = NULL;
        f->ix[k] = NULL;
    }
    for (int k = 0; k < 8; ++k) {
        free(f->bands[k]);
        f->bands[k] = NULL;
    }
    free(f->buf.tmp_ref);
    f->buf.tmp_ref = NULL;
    free(f->src);
    f->src = NULL;
}

static void fixture_bind(Dwt2Fixture *f)
{
    for (int k = 0; k < 4; ++k) {
        f->buf.ind_y[k] = f->iy[k];
        f->buf.ind_x[k] = f->ix[k];
    }
    f->ref_band.band_a = f->bands[0];
    f->ref_band.band_v = f->bands[1];
    f->ref_band.band_h = f->bands[2];
    f->ref_band.band_d = f->bands[3];
    f->simd_band.band_a = f->bands[4];
    f->simd_band.band_v = f->bands[5];
    f->simd_band.band_h = f->bands[6];
    f->simd_band.band_d = f->bands[7];
}

/* Allocates every buffer, or frees whatever was allocated and returns -1. The
 * NEON bands start out filled with the guard pattern. */
static int fixture_alloc(Dwt2Fixture *f, int w, int h)
{
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->w_half = (w + 1) / 2;
    f->h_half = (h + 1) / 2;
    /* integer_adm.c: buf_stride = ALIGN_CEIL(half width * 4) >> 2. */
    f->dst_stride = ALIGN_CEIL(f->w_half * (int)sizeof(int32_t)) / (int)sizeof(int32_t);
    f->band_elems = (size_t)f->h_half * (size_t)f->dst_stride;
    f->tmp_elems = ((size_t)w * 8) + 256;

    int ok = 1;
    f->src = malloc((size_t)w * (size_t)h);
    ok = ok && (f->src != NULL);
    for (int k = 0; k < 4; ++k) {
        f->iy[k] = calloc((size_t)f->h_half + 64, sizeof(int));
        f->ix[k] = calloc((size_t)f->w_half + 64, sizeof(int));
        ok = ok && (f->iy[k] != NULL) && (f->ix[k] != NULL);
    }
    for (int k = 0; k < 8; ++k) {
        f->bands[k] = calloc(f->band_elems + DWT2_SLACK, sizeof(int16_t));
        ok = ok && (f->bands[k] != NULL);
        if (ok && k >= 4)
            simd_test_guard_fill(f->bands[k], (f->band_elems + DWT2_SLACK) * sizeof(int16_t));
    }
    f->buf.tmp_ref = calloc(f->tmp_elems, sizeof(int16_t));
    ok = ok && (f->buf.tmp_ref != NULL);

    if (!ok) {
        fixture_free(f);
        return -1;
    }
    fixture_bind(f);
    return 0;
}

/* xorshift32 -- deterministic per (w, h), so a failure is reproducible. */
static void fixture_fill_src(Dwt2Fixture *f)
{
    uint32_t seed = 0x5eed0000u ^ (uint32_t)((f->w * 131) + f->h);

    for (int i = 0; i < f->w * f->h; ++i)
        f->src[i] = (uint8_t)(simd_test_xorshift32(&seed) & 0xFFu);
}

/* Returns 1 when every in-band sample matches; otherwise reports the first
 * mismatch on stderr and returns 0. */
static int fixture_bands_match(const Dwt2Fixture *f)
{
    static const char *const names[4] = {"band_a", "band_v", "band_h", "band_d"};

    for (int b = 0; b < 4; ++b) {
        for (size_t idx = 0; idx < f->band_elems; ++idx) {
            const int i = (int)(idx / (size_t)f->dst_stride);
            const int j = (int)(idx % (size_t)f->dst_stride);
            if (j >= f->w_half || f->bands[b][idx] == f->bands[b + 4][idx])
                continue;
            (void)fprintf(stderr, "  %dx%d %s[%d][%d]%s: scalar %d != neon %d\n", f->w, f->h,
                          names[b], i, j, (j == f->w_half - 1) ? " (last col)" : "",
                          f->bands[b][idx], f->bands[b + 4][idx]);
            return 0;
        }
    }
    return 1;
}

/* Elements of the NEON bands written outside the h_half x w_half band. */
static size_t fixture_guard_touched(const Dwt2Fixture *f)
{
    const SimdTestRect rect = {0, (size_t)f->h_half, 0, (size_t)f->w_half};
    size_t touched = 0;

    for (int b = 4; b < 8; ++b) {
        const SimdTestPlane plane = {f->bands[b], sizeof(int16_t), (size_t)f->dst_stride,
                                     (size_t)f->h_half, DWT2_SLACK};
        touched += simd_test_guard_count_outside(plane, rect);
    }
    return touched;
}

/* One (w, h) scalar-vs-NEON comparison plus the guard band. */
static char *dwt2_geometry_matches_scalar(int w, int h)
{
    Dwt2Fixture f;

    mu_assert("allocation failed for the ADM DWT2 fixture", fixture_alloc(&f, w, h) == 0);
    fixture_fill_src(&f);
    ref_src_indices(f.buf.ind_y, f.buf.ind_x, w, h);

    ref_adm_dwt2_8(f.src, &f.ref_band, &f.buf, w, h, w, f.dst_stride);
    memset(f.buf.tmp_ref, 0, f.tmp_elems * sizeof(int16_t));
    adm_dwt2_8_neon(f.src, &f.simd_band, &f.buf, w, h, w, f.dst_stride);

    const int matched = fixture_bands_match(&f);
    const size_t touched = fixture_guard_touched(&f);
    fixture_free(&f);
    if (touched != 0)
        (void)fprintf(stderr, "  %dx%d\n", w, h);
    mu_assert("adm_dwt2_8_neon diverges from the scalar reference", matched);
    SIMD_GUARD_ASSERT_UNTOUCHED(touched, "adm_dwt2_8_neon wrote outside its band");
    return NULL;
}

/* The 32-bit DWT of scales 1-3: two int32 source planes, both pictures'
 * four bands for the scalar side (out[0..7]) and for NEON (out[8..15]). */
typedef struct I4Dwt2Fixture {
    int w;
    int h;
    int w_half;
    int h_half;
    int dst_stride;
    size_t band_elems;
    int32_t *src[2];
    int32_t *out[16];
    int *iy[4];
    int *ix[4];
    AdmBuffer buf;
} I4Dwt2Fixture;

static void i4_fixture_free(I4Dwt2Fixture *f)
{
    for (int k = 0; k < 4; ++k) {
        free(f->iy[k]);
        free(f->ix[k]);
    }
    for (int k = 0; k < 16; ++k) {
        free(f->out[k]);
    }
    free(f->src[0]);
    free(f->src[1]);
    free(f->buf.tmp_ref);
    memset(f, 0, sizeof(*f));
}

/* Points the AdmBuffer's 32-bit DWT bands at the scalar (`side` 0) or the
 * NEON (`side` 1) outputs. */
static void i4_fixture_bind(I4Dwt2Fixture *f, int side)
{
    int32_t *const *o = f->out + ((ptrdiff_t)8 * side);
    f->buf.i4_ref_dwt2 =
        (i4_adm_dwt_band_t){.band_a = o[0], .band_v = o[1], .band_h = o[2], .band_d = o[3]};
    f->buf.i4_dis_dwt2 =
        (i4_adm_dwt_band_t){.band_a = o[4], .band_v = o[5], .band_h = o[6], .band_d = o[7]};
}

static int i4_fixture_alloc(I4Dwt2Fixture *f, int w, int h)
{
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->w_half = (w + 1) / 2;
    f->h_half = (h + 1) / 2;
    f->dst_stride = ALIGN_CEIL(f->w_half * (int)sizeof(int32_t)) / (int)sizeof(int32_t);
    f->band_elems = (size_t)f->h_half * (size_t)f->dst_stride;

    int ok = 1;
    for (int p = 0; p < 2; ++p) {
        f->src[p] = malloc((size_t)w * (size_t)h * sizeof(int32_t));
        ok = ok && (f->src[p] != NULL);
    }
    for (int k = 0; k < 4; ++k) {
        f->iy[k] = calloc((size_t)f->h_half + 64, sizeof(int));
        f->ix[k] = calloc((size_t)f->w_half + 64, sizeof(int));
        ok = ok && (f->iy[k] != NULL) && (f->ix[k] != NULL);
        f->buf.ind_y[k] = f->iy[k];
        f->buf.ind_x[k] = f->ix[k];
    }
    for (int k = 0; k < 16; ++k) {
        f->out[k] = calloc(f->band_elems + DWT2_SLACK, sizeof(int32_t));
        ok = ok && (f->out[k] != NULL);
        if (ok && k >= 8)
            simd_test_guard_fill(f->out[k], (f->band_elems + DWT2_SLACK) * sizeof(int32_t));
    }
    /* adm_dwt2_s123_combined(): tmplo / tmphi of both pictures, w each. */
    f->buf.tmp_ref = calloc((size_t)w * 4, sizeof(int32_t));
    ok = ok && (f->buf.tmp_ref != NULL);
    if (!ok) {
        i4_fixture_free(f);
        return -1;
    }
    return 0;
}

/* Full-range int32 samples, deterministic per (w, h, scale). */
static void i4_fixture_fill_src(I4Dwt2Fixture *f, int scale)
{
    uint32_t seed = 0x51230000u ^ (uint32_t)((f->w * 131) + (f->h * 7) + scale);
    for (int p = 0; p < 2; ++p) {
        for (int i = 0; i < f->w * f->h; ++i)
            f->src[p][i] = (int32_t)simd_test_xorshift32(&seed);
    }
}

/* adm_dwt2_s123_combined() of integer_adm.c, from its kernels. */
static void i4_ref_dwt2_s123(I4Dwt2Fixture *f, int scale)
{
    const I4Dwt2Round r = i4_dwt2_round(scale);
    for (int i = 0; i < f->h_half; ++i) {
        i4_dwt2_vpass(f->src[0], f->src[1], f->buf.ind_y, i, f->w, f->w, f->w, 0, f->w,
                      f->buf.tmp_ref, r.add_vp, r.shift_vp);
        i4_dwt2_hpass(f->buf.tmp_ref, &f->buf.i4_ref_dwt2, &f->buf.i4_dis_dwt2, f->buf.ind_x, i,
                      f->w, 0, f->w_half, f->dst_stride, r.add_hp, r.shift_hp);
    }
}

/* In-band mismatches between the scalar and the NEON bands, plus NEON
 * elements written outside the h_half x w_half band. */
static size_t i4_fixture_bad(const I4Dwt2Fixture *f, size_t *touched)
{
    const SimdTestRect rect = {0, (size_t)f->h_half, 0, (size_t)f->w_half};
    size_t bad = 0;
    *touched = 0;
    for (int b = 0; b < 8; ++b) {
        for (size_t idx = 0; idx < f->band_elems; ++idx) {
            const bool in_band = (int)(idx % (size_t)f->dst_stride) < f->w_half;
            bad += (in_band && f->out[b][idx] != f->out[b + 8][idx]) ? 1u : 0u;
        }
        const SimdTestPlane plane = {f->out[b + 8], sizeof(int32_t), (size_t)f->dst_stride,
                                     (size_t)f->h_half, DWT2_SLACK};
        *touched += simd_test_guard_count_outside(plane, rect);
    }
    return bad;
}

static char *dwt2_s123_geometry_matches_scalar(int w, int h, int scale)
{
    I4Dwt2Fixture f;

    mu_assert("allocation failed for the 32-bit DWT2 fixture", i4_fixture_alloc(&f, w, h) == 0);
    i4_fixture_fill_src(&f, scale);
    ref_src_indices(f.buf.ind_y, f.buf.ind_x, w, h);
    i4_fixture_bind(&f, 0);
    i4_ref_dwt2_s123(&f, scale);
    memset(f.buf.tmp_ref, 0, (size_t)w * 4 * sizeof(int32_t));
    i4_fixture_bind(&f, 1);
    adm_dwt2_s123_combined_neon(f.src[0], f.src[1], &f.buf, w, h, w, w, f.dst_stride, scale);

    size_t touched = 0;
    const size_t bad = i4_fixture_bad(&f, &touched);
    i4_fixture_free(&f);
    if (bad != 0 || touched != 0)
        (void)fprintf(stderr, "  %dx%d scale %d: %zu samples differ\n", w, h, scale, bad);
    mu_assert("adm_dwt2_s123_combined_neon diverges from the scalar reference", bad == 0);
    SIMD_GUARD_ASSERT_UNTOUCHED(touched, "adm_dwt2_s123_combined_neon wrote outside its band");
    return NULL;
}
#endif /* ARCH_AARCH64 */

static char *test_adm_dwt2_8_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    /* Only widths the dispatcher routes to NEON: integer_adm.c gates on
     * `!(w % 8)`, so both w % 16 == 0 and w % 16 == 8 are in scope. The heights
     * include 17 (the extractor minimum) and odd extents for the mirrored tail. */
    static const int heights[] = {16, 17, 18, 24, 33, 48};

    for (size_t t = 0; t < sizeof(heights) / sizeof(heights[0]); ++t) {
        for (int w = 16; w <= 128; w += 8) {
            char *msg = dwt2_geometry_matches_scalar(w, heights[t]);
            if (msg)
                return msg;
        }
    }
    return dwt2_geometry_matches_scalar(576, 32); /* a Netflix golden width */
#endif
}

static char *test_adm_dwt2_s123_neon_matches_scalar(void)
{
#if !ARCH_AARCH64
    return NULL; /* NEON kernel is aarch64-only. */
#else
    /* Every width from 3 (a scale-3 input of a 17-pixel frame) to 70, so the
     * four-wide vertical loop and the de-interleaved horizontal blocks meet
     * every tail and every mirrored last column. */
    static const int heights[] = {3, 4, 7, 12};

    for (int scale = 1; scale <= 3; ++scale) {
        for (size_t t = 0; t < sizeof(heights) / sizeof(heights[0]); ++t) {
            for (int w = 3; w <= 70; ++w) {
                char *msg = dwt2_s123_geometry_matches_scalar(w, heights[t], scale);
                if (msg)
                    return msg;
            }
        }
    }
    return dwt2_s123_geometry_matches_scalar(288, 18, 1); /* the Netflix pair's scale 1 */
#endif
}

char *run_tests(void)
{
#if ARCH_AARCH64
    mu_run_test(test_adm_dwt2_8_neon_matches_scalar);
    mu_run_test(test_adm_dwt2_s123_neon_matches_scalar);
#else
    (void)fprintf(stderr, "skipping: non-aarch64 arch\n");
    (void)test_adm_dwt2_8_neon_matches_scalar;
    (void)test_adm_dwt2_s123_neon_matches_scalar;
#endif
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
