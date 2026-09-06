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

#include <stddef.h>
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

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
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
                u[t] = src[(ind_y[t][i] * src_stride) + j];

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
            dst->band_a[(i * dst_stride) + j] = (int16_t)((accum + add_HP) >> shift_HP);
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)fhi[t] * s[t];
            dst->band_v[(i * dst_stride) + j] = (int16_t)((accum + add_HP) >> shift_HP);

            for (int t = 0; t < 4; ++t)
                s[t] = tmphi[jx[t]];
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)flo[t] * s[t];
            dst->band_h[(i * dst_stride) + j] = (int16_t)((accum + add_HP) >> shift_HP);
            accum = 0;
            for (int t = 0; t < 4; ++t)
                accum += (int32_t)fhi[t] * s[t];
            dst->band_d[(i * dst_stride) + j] = (int16_t)((accum + add_HP) >> shift_HP);
        }
    }
}

/* Signature shared by adm_dwt2_8_avx2() and adm_dwt2_8_avx512(). */
typedef void (*adm_dwt2_8_fn)(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                              int h, int src_stride, int dst_stride);

/* Everything one (w, h) comparison needs. bands[0..3] receive the scalar
 * reference, bands[4..7] the SIMD kernel, so band b and band b + 4 hold the
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

/* Allocates every buffer, or frees whatever was allocated and returns -1. */
static int fixture_alloc(Dwt2Fixture *f, int w, int h)
{
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->w_half = (w + 1) / 2;
    f->h_half = (h + 1) / 2;
    f->dst_stride = f->w_half;
    f->band_elems = (size_t)f->h_half * (size_t)f->dst_stride;
    f->tmp_elems = ((size_t)w * 8) + 256;

    int ok = 1;
    f->src = malloc((size_t)w * (size_t)h);
    ok = ok && (f->src != NULL);
    for (int k = 0; k < 4; ++k) {
        f->iy[k] = calloc((size_t)f->h_half + 64, sizeof(int));
        f->ix[k] = calloc((size_t)f->w_half + 64, sizeof(int));
        ok = ok && (f->iy[k] != NULL) && (f->ix[k] != NULL);
        f->buf.ind_y[k] = f->iy[k];
        f->buf.ind_x[k] = f->ix[k];
    }
    for (int k = 0; k < 8; ++k) {
        f->bands[k] = calloc(f->band_elems + 64, sizeof(int16_t));
        ok = ok && (f->bands[k] != NULL);
    }
    f->buf.tmp_ref = calloc(f->tmp_elems, sizeof(int16_t));
    ok = ok && (f->buf.tmp_ref != NULL);

    if (!ok) {
        fixture_free(f);
        return -1;
    }

    f->ref_band.band_a = f->bands[0];
    f->ref_band.band_v = f->bands[1];
    f->ref_band.band_h = f->bands[2];
    f->ref_band.band_d = f->bands[3];
    f->simd_band.band_a = f->bands[4];
    f->simd_band.band_v = f->bands[5];
    f->simd_band.band_h = f->bands[6];
    f->simd_band.band_d = f->bands[7];
    return 0;
}

/* xorshift32 -- deterministic per (w, h), so a failure is reproducible. */
static void fixture_fill_src(Dwt2Fixture *f)
{
    uint32_t seed = 0x5eed0000u ^ (uint32_t)((f->w * 131) + f->h);

    for (int i = 0; i < f->w * f->h; ++i) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        f->src[i] = (uint8_t)(seed & 0xFFu);
    }
}

/* Returns 1 when every band matches; otherwise reports the first mismatch on
 * stderr and returns 0. */
static int fixture_bands_match(const Dwt2Fixture *f, const char *label)
{
    static const char *const names[4] = {"band_a", "band_v", "band_h", "band_d"};

    for (int b = 0; b < 4; ++b) {
        for (size_t idx = 0; idx < f->band_elems; ++idx) {
            if (f->bands[b][idx] == f->bands[b + 4][idx])
                continue;
            const int i = (int)(idx / (size_t)f->dst_stride);
            const int j = (int)(idx % (size_t)f->dst_stride);
            (void)fprintf(stderr, "  %s %dx%d %s[%d][%d]%s: scalar %d != simd %d\n", label, f->w,
                          f->h, names[b], i, j, (j == f->w_half - 1) ? " (last col)" : "",
                          f->bands[b][idx], f->bands[b + 4][idx]);
            return 0;
        }
    }
    return 1;
}

/* Widths chosen so that (w + 1) / 2 % N == 1 for the vector widths the AVX2 and
 * AVX-512 DWT2 kernels use (N in {4, 8, 16, 32, 64}) -- exactly the case the
 * old `half_w - ((half_w - 1) % N)` bound handed to the vector loop instead of
 * the scalar mirror tail. 576 is a control (a Netflix golden width). */
static char *dwt2_kernel_matches_scalar(const char *label, adm_dwt2_8_fn kernel)
{
    static const int widths[] = {34, 66, 130, 258, 576};
    const int h = 32;

    for (size_t t = 0; t < sizeof(widths) / sizeof(widths[0]); ++t) {
        Dwt2Fixture f;
        const int w = widths[t];

        if (fixture_alloc(&f, w, h) != 0)
            return "allocation failed for the ADM DWT2 fixture";

        fixture_fill_src(&f);
        ref_src_indices(f.buf.ind_y, f.buf.ind_x, w, h);

        ref_adm_dwt2_8(f.src, &f.ref_band, &f.buf, w, h, w, f.dst_stride);
        memset(f.buf.tmp_ref, 0, f.tmp_elems * sizeof(int16_t));
        kernel(f.src, &f.simd_band, &f.buf, w, h, w, f.dst_stride);

        const int matched = fixture_bands_match(&f, label);
        fixture_free(&f);
        if (!matched)
            return "the x86 ADM DWT2 kernel diverges from the scalar reference";
    }
    return NULL;
}
#endif /* ARCH_X86 */

static char *test_adm_dwt2_8_avx2_matches_scalar(void)
{
#if !ARCH_X86
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2))
        return NULL;
    return dwt2_kernel_matches_scalar("AVX2", adm_dwt2_8_avx2);
#endif
}

static char *test_adm_dwt2_8_avx512_matches_scalar(void)
{
#if !(ARCH_X86 && HAVE_AVX512)
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512))
        return NULL;
    return dwt2_kernel_matches_scalar("AVX512", adm_dwt2_8_avx512);
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
    return NULL;
}

/* NOLINTEND(modernize-use-nullptr) */
