/**
 * Copyright 2016-2020 Netflix, Inc.
 * Copyright 2026 Lusoris
 *
 * SPDX-License-Identifier: BSD-2-Clause-Patent
 *
 * x86-vs-scalar bit-exactness for the integer ADM scale-0 DWT2 kernels.
 *
 * Regression test for x86 half_w_modN tail bound defect (T-UPSTREAM-1564).
 * Tests scalar adm_dwt2_8 against adm_dwt2_8_avx2 and adm_dwt2_8_avx512, and
 * scalar adm_dwt2_16 (at 10-bit and 12-bit input depths) against
 * adm_dwt2_16_avx2 and adm_dwt2_16_avx512, on widths where
 * (w + 1) / 2 % N == 1, ensuring the last column is correctly handled by the
 * scalar mirror path and not the vector contiguous load path.
 *
 * The SIMD bands use the band stride integer_adm.c derives and start out
 * filled with the simd_bitexact_test.h guard pattern, so a store outside the
 * half-resolution band (into the stride padding, the next row, or past the
 * band) fails the test even when the in-band samples match. A small-size
 * sweep covers every frame width from 17 to 96.
 */

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
#include "cpu.h"
#include "mem.h"

#include "feature/integer_adm.h"

/* NOLINTBEGIN(modernize-use-nullptr): C translation unit. The fork builds C as
 * C23, where clang-tidy also proposes the `nullptr` keyword, but MSVC's
 * documented /std:clatest C23 feature set does not include `nullptr` while the
 * required Windows build compiles this TU with cl.exe, and this test mirrors
 * the C spelling of the surface it exercises. ADR-1138. */

#if ARCH_X86
#include "feature/x86/adm_avx2.h"
#if HAVE_AVX512
#include "feature/x86/adm_avx512.h"
#endif

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
            if (idx[t] >= len) {
                idx[t] = (2 * len) - idx[t] - 1;
            }
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

static int32_t ref_dwt2_tap4(const int16_t *f, int32_t s0, int32_t s1, int32_t s2, int32_t s3)
{
    int32_t accum = (int32_t)f[0] * s0;
    accum += (int32_t)f[1] * s1;
    accum += (int32_t)f[2] * s2;
    accum += (int32_t)f[3] * s3;
    return accum;
}

/* Vertical-pass 4-tap filter of adm_dwt2_vpass_16() / adm_dwt2_vpass16_tap4(). */
static int16_t ref_dwt2_vpass16_tap4(const int16_t *f, int32_t f_sum, const uint16_t u[4],
                                     int32_t add_vp, int shift_vp)
{
    int64_t accum = (int64_t)f[0] * (int64_t)u[0];
    accum += (int64_t)f[1] * (int64_t)u[1];
    accum += (int64_t)f[2] * (int64_t)u[2];
    accum += (int64_t)f[3] * (int64_t)u[3];
    accum -= (int64_t)f_sum * (int64_t)add_vp;
    return (int16_t)(int32_t)((accum + (int64_t)add_vp) >> shift_vp);
}

/* Horizontal pass of one output row (adm_dwt2_hpass() in integer_adm_kernels.h). */
static void ref_dwt2_hpass_row(const int16_t *tmplo, const int16_t *tmphi,
                               const adm_dwt_band_t *dst, int *const *ind_x, int i, int w_half,
                               int dst_stride)
{
    const int16_t *flo = dwt2_db2_coeffs_lo;
    const int16_t *fhi = dwt2_db2_coeffs_hi;
    const int16_t shift_hp = 16;
    const int32_t add_hp = 32768;
    const ptrdiff_t row = (ptrdiff_t)i * dst_stride;

    for (int j = 0; j < w_half; ++j) {
        const int jx0 = ind_x[0][j];
        const int jx1 = ind_x[1][j];
        const int jx2 = ind_x[2][j];
        const int jx3 = ind_x[3][j];
        int32_t accum = ref_dwt2_tap4(flo, tmplo[jx0], tmplo[jx1], tmplo[jx2], tmplo[jx3]);
        dst->band_a[row + j] = (int16_t)((accum + add_hp) >> shift_hp);
        accum = ref_dwt2_tap4(fhi, tmplo[jx0], tmplo[jx1], tmplo[jx2], tmplo[jx3]);
        dst->band_v[row + j] = (int16_t)((accum + add_hp) >> shift_hp);
        accum = ref_dwt2_tap4(flo, tmphi[jx0], tmphi[jx1], tmphi[jx2], tmphi[jx3]);
        dst->band_h[row + j] = (int16_t)((accum + add_hp) >> shift_hp);
        accum = ref_dwt2_tap4(fhi, tmphi[jx0], tmphi[jx1], tmphi[jx2], tmphi[jx3]);
        dst->band_d[row + j] = (int16_t)((accum + add_hp) >> shift_hp);
    }
}

/* Transcribed from adm_dwt2_8() in integer_adm.c (static there). */
static void ref_adm_dwt2_8(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                           int h, int src_stride, int dst_stride)
{
    const int16_t *flo = dwt2_db2_coeffs_lo;
    const int16_t *fhi = dwt2_db2_coeffs_hi;
    const int16_t shift_vp = 8;
    const int32_t add_vp = 128;
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        for (int j = 0; j < w; ++j) {
            const int32_t u0 = src[((ptrdiff_t)ind_y[0][i] * src_stride) + j];
            const int32_t u1 = src[((ptrdiff_t)ind_y[1][i] * src_stride) + j];
            const int32_t u2 = src[((ptrdiff_t)ind_y[2][i] * src_stride) + j];
            const int32_t u3 = src[((ptrdiff_t)ind_y[3][i] * src_stride) + j];
            int32_t accum = ref_dwt2_tap4(flo, u0, u1, u2, u3);
            accum -= dwt2_db2_coeffs_lo_sum * add_vp;
            tmplo[j] = (int16_t)((accum + add_vp) >> shift_vp);
            accum = ref_dwt2_tap4(fhi, u0, u1, u2, u3);
            accum -= dwt2_db2_coeffs_hi_sum * add_vp;
            tmphi[j] = (int16_t)((accum + add_vp) >> shift_vp);
        }
        ref_dwt2_hpass_row(tmplo, tmphi, dst, ind_x, i, (w + 1) / 2, dst_stride);
    }
}

/* Transcribed from adm_dwt2_16() in integer_adm.c (static there). `src_stride`
 * counts uint16_t elements (integer_adm.c adm_src_stride(): pic->stride[0] >> 1). */
static void ref_adm_dwt2_16(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                            int h, int src_stride, int dst_stride, int inp_size_bits)
{
    const int16_t *flo = dwt2_db2_coeffs_lo;
    const int16_t *fhi = dwt2_db2_coeffs_hi;
    const int shift_vp = inp_size_bits;
    const int32_t add_vp = (int32_t)(1u << (unsigned)(inp_size_bits - 1));
    int **ind_y = buf->ind_y;
    int **ind_x = buf->ind_x;
    int16_t *tmplo = (int16_t *)buf->tmp_ref;
    int16_t *tmphi = tmplo + w;

    for (int i = 0; i < (h + 1) / 2; ++i) {
        for (int j = 0; j < w; ++j) {
            const uint16_t u[4] = {
                src[((ptrdiff_t)ind_y[0][i] * src_stride) + j],
                src[((ptrdiff_t)ind_y[1][i] * src_stride) + j],
                src[((ptrdiff_t)ind_y[2][i] * src_stride) + j],
                src[((ptrdiff_t)ind_y[3][i] * src_stride) + j],
            };
            tmplo[j] = ref_dwt2_vpass16_tap4(flo, dwt2_db2_coeffs_lo_sum, u, add_vp, shift_vp);
            tmphi[j] = ref_dwt2_vpass16_tap4(fhi, dwt2_db2_coeffs_hi_sum, u, add_vp, shift_vp);
        }
        ref_dwt2_hpass_row(tmplo, tmphi, dst, ind_x, i, (w + 1) / 2, dst_stride);
    }
}

/* Signatures shared by the AVX2 and AVX-512 8-bit and 16-bit DWT2 kernels. */
typedef void (*adm_dwt2_8_fn)(const uint8_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf, int w,
                              int h, int src_stride, int dst_stride);
typedef void (*adm_dwt2_16_fn)(const uint16_t *src, const adm_dwt_band_t *dst, AdmBuffer *buf,
                               int w, int h, int src_stride, int dst_stride, int inp_size_bits);

typedef struct Dwt2KernelSpec {
    const char *label;
    adm_dwt2_8_fn fn8;
    adm_dwt2_16_fn fn16;
} Dwt2KernelSpec;

/* Trailing elements past each band that no kernel may write. */
#define DWT2_SLACK 64

/* Everything one (w, h, bits) comparison needs. bands[0..3] receive the scalar
 * reference, bands[4..7] the SIMD kernel, so band b and band b + 4 hold the
 * two implementations of the same band. */
typedef struct Dwt2Fixture {
    int w;
    int h;
    int bits;
    int w_half;
    int h_half;
    int dst_stride;
    size_t band_elems;
    size_t tmp_elems;
    uint8_t *src;
    uint16_t *src16;
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
    free(f->src16);
    f->src16 = NULL;
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

/* Allocates every buffer for an 8-bit or 16-bit source, or frees whatever was
 * allocated and returns -1. */
static int fixture_alloc(Dwt2Fixture *f, int w, int h, int bits)
{
    memset(f, 0, sizeof(*f));
    f->w = w;
    f->h = h;
    f->bits = bits;
    f->w_half = (w + 1) / 2;
    f->h_half = (h + 1) / 2;
    /* integer_adm.c: buf_stride = ALIGN_CEIL(half width * 4) >> 2. */
    f->dst_stride = ALIGN_CEIL(f->w_half * (int)sizeof(int32_t)) / (int)sizeof(int32_t);
    f->band_elems = (size_t)f->h_half * (size_t)f->dst_stride;
    f->tmp_elems = ((size_t)w * 8) + 256;

    unsigned failed = 0u;
    if (bits > 8) {
        f->src16 = malloc((size_t)w * (size_t)h * sizeof(uint16_t));
        failed |= (unsigned)(f->src16 == NULL);
    } else {
        f->src = malloc((size_t)w * (size_t)h);
        failed |= (unsigned)(f->src == NULL);
    }
    for (int k = 0; k < 4; ++k) {
        f->iy[k] = calloc((size_t)f->h_half + 64, sizeof(int));
        f->ix[k] = calloc((size_t)f->w_half + 64, sizeof(int));
        failed |= (unsigned)(f->iy[k] == NULL) | (unsigned)(f->ix[k] == NULL);
    }
    for (int k = 0; k < 8; ++k) {
        f->bands[k] = calloc(f->band_elems + DWT2_SLACK, sizeof(int16_t));
        failed |= (unsigned)(f->bands[k] == NULL);
        if (k >= 4 && f->bands[k] != NULL) {
            simd_test_guard_fill(f->bands[k], (f->band_elems + DWT2_SLACK) * sizeof(int16_t));
        }
    }
    f->buf.tmp_ref = calloc(f->tmp_elems, sizeof(int16_t));
    failed |= (unsigned)(f->buf.tmp_ref == NULL);

    if (failed != 0u) {
        fixture_free(f);
        return -1;
    }
    fixture_bind(f);
    return 0;
}

/* xorshift32 -- deterministic per (w, h, bits), with samples in [0, 2^bits - 1]. */
static void fixture_fill_src(Dwt2Fixture *f)
{
    uint32_t seed = 0x5eed0000u ^ (uint32_t)((f->w * 131) + f->h + ((f->bits - 8) * 977));
    const uint32_t mask = (1u << (unsigned)f->bits) - 1u;
    const int count = f->w * f->h;

    if (f->bits > 8) {
        for (int i = 0; i < count; ++i) {
            f->src16[i] = (uint16_t)(simd_test_xorshift32(&seed) & mask);
        }
    } else {
        for (int i = 0; i < count; ++i) {
            f->src[i] = (uint8_t)(simd_test_xorshift32(&seed) & mask);
        }
    }
}

/* Returns 1 when every band matches; otherwise reports the first mismatch on
 * stderr and returns 0. */
static int fixture_bands_match(const Dwt2Fixture *f, const char *label)
{
    static const char *const names[4] = {"band_a", "band_v", "band_h", "band_d"};

    for (int b = 0; b < 4; ++b) {
        for (size_t idx = 0; idx < f->band_elems; ++idx) {
            const int i = (int)(idx / (size_t)f->dst_stride);
            const int j = (int)(idx % (size_t)f->dst_stride);
            if (j >= f->w_half || f->bands[b][idx] == f->bands[b + 4][idx]) {
                continue;
            }
            (void)fprintf(stderr, "  %s %dbpc %dx%d %s[%d][%d]%s: scalar %d != simd %d\n", label,
                          f->bits, f->w, f->h, names[b], i, j,
                          (j == f->w_half - 1) ? " (last col)" : "", f->bands[b][idx],
                          f->bands[b + 4][idx]);
            return 0;
        }
    }
    return 1;
}

/* Elements of the SIMD bands written outside the h_half x w_half band. */
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

/* One (w, h, bits) scalar-vs-SIMD comparison plus the guard band. */
static char *dwt2_geometry_matches_scalar(const Dwt2KernelSpec *spec, int w, int h, int bits)
{
    Dwt2Fixture f;

    mu_assert("allocation failed for the ADM DWT2 fixture", fixture_alloc(&f, w, h, bits) == 0);
    fixture_fill_src(&f);
    ref_src_indices(f.buf.ind_y, f.buf.ind_x, w, h);

    if (bits > 8) {
        ref_adm_dwt2_16(f.src16, &f.ref_band, &f.buf, w, h, w, f.dst_stride, bits);
        memset(f.buf.tmp_ref, 0, f.tmp_elems * sizeof(int16_t));
        spec->fn16(f.src16, &f.simd_band, &f.buf, w, h, w, f.dst_stride, bits);
    } else {
        ref_adm_dwt2_8(f.src, &f.ref_band, &f.buf, w, h, w, f.dst_stride);
        memset(f.buf.tmp_ref, 0, f.tmp_elems * sizeof(int16_t));
        spec->fn8(f.src, &f.simd_band, &f.buf, w, h, w, f.dst_stride);
    }

    const int matched = fixture_bands_match(&f, spec->label);
    const size_t touched = fixture_guard_touched(&f);
    fixture_free(&f);
    if (touched != 0) {
        (void)fprintf(stderr, "  %s %dbpc %dx%d\n", spec->label, bits, w, h);
    }
    mu_assert("the x86 ADM DWT2 kernel diverges from the scalar reference", matched);
    SIMD_GUARD_ASSERT_UNTOUCHED(touched, "the x86 ADM DWT2 kernel wrote outside its band");
    return NULL;
}

/* Widths chosen so that (w + 1) / 2 % N == 1 for the vector widths the AVX2 and
 * AVX-512 DWT2 kernels use (N in {4, 8, 16, 32, 64}) -- exactly the case the
 * old `half_w - ((half_w - 1) % N)` bound handed to the vector loop instead of
 * the scalar mirror tail, covering every width where the 8-bit and 16-bit
 * kernels diverged -- plus 576 as a control (a Netflix golden width). */
static char *dwt2_kernel_matches_scalar(const Dwt2KernelSpec *spec, int bits)
{
    static const int widths[] = {34, 66, 97, 98, 129, 130, 161, 162, 257, 258, 576};
    /* Small-size sweep: every frame width the extractor accepts up to 96, over
     * heights that put the mirrored last row at every phase. */
    static const int sweep_heights[] = {17, 18, 24, 33};

    for (size_t t = 0; t < sizeof(widths) / sizeof(widths[0]); ++t) {
        char *msg = dwt2_geometry_matches_scalar(spec, widths[t], 32, bits);
        if (msg != NULL) {
            return msg;
        }
    }
    for (size_t t = 0; t < sizeof(sweep_heights) / sizeof(sweep_heights[0]); ++t) {
        for (int w = 17; w <= 96; ++w) {
            char *msg = dwt2_geometry_matches_scalar(spec, w, sweep_heights[t], bits);
            if (msg != NULL) {
                return msg;
            }
        }
    }
    return NULL;
}

static char *dwt2_16_kernel_matches_scalar(const Dwt2KernelSpec *spec)
{
    static const int bit_depths[] = {10, 12};

    for (size_t d = 0; d < sizeof(bit_depths) / sizeof(bit_depths[0]); ++d) {
        char *msg = dwt2_kernel_matches_scalar(spec, bit_depths[d]);
        if (msg != NULL) {
            return msg;
        }
    }
    return NULL;
}

/* Each kernel runs and reports even when an earlier one failed. */
static char *report_all(const char *name, char *(*test)(void), char *first)
{
    char *r = mu_report(name, test);
    return (first != NULL) ? first : r;
}
#endif /* ARCH_X86 */

static char *test_adm_dwt2_8_avx2_matches_scalar(void)
{
#if !ARCH_X86
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2)) {
        (void)fprintf(stderr, "skipping: adm_dwt2_8_avx2: no AVX2\n");
        return NULL;
    }
    const Dwt2KernelSpec spec = {"adm_dwt2_8_avx2", adm_dwt2_8_avx2, NULL};
    return dwt2_kernel_matches_scalar(&spec, 8);
#endif
}

static char *test_adm_dwt2_8_avx512_matches_scalar(void)
{
#if !(ARCH_X86 && HAVE_AVX512)
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512)) {
        (void)fprintf(stderr, "skipping: adm_dwt2_8_avx512: no AVX-512\n");
        return NULL;
    }
    const Dwt2KernelSpec spec = {"adm_dwt2_8_avx512", adm_dwt2_8_avx512, NULL};
    return dwt2_kernel_matches_scalar(&spec, 8);
#endif
}

static char *test_adm_dwt2_16_avx2_matches_scalar(void)
{
#if !ARCH_X86
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX2)) {
        (void)fprintf(stderr, "skipping: adm_dwt2_16_avx2: no AVX2\n");
        return NULL;
    }
    const Dwt2KernelSpec spec = {"adm_dwt2_16_avx2", NULL, adm_dwt2_16_avx2};
    return dwt2_16_kernel_matches_scalar(&spec);
#endif
}

static char *test_adm_dwt2_16_avx512_matches_scalar(void)
{
#if !(ARCH_X86 && HAVE_AVX512)
    return NULL;
#else
    if (!(vmaf_get_cpu_flags() & VMAF_X86_CPU_FLAG_AVX512)) {
        (void)fprintf(stderr, "skipping: adm_dwt2_16_avx512: no AVX-512\n");
        return NULL;
    }
    const Dwt2KernelSpec spec = {"adm_dwt2_16_avx512", NULL, adm_dwt2_16_avx512};
    return dwt2_16_kernel_matches_scalar(&spec);
#endif
}

char *run_tests(void)
{
    char *first = NULL;
#if ARCH_X86
    vmaf_init_cpu();
    first = report_all("test_adm_dwt2_8_avx2_matches_scalar", test_adm_dwt2_8_avx2_matches_scalar,
                       first);
    first = report_all("test_adm_dwt2_16_avx2_matches_scalar", test_adm_dwt2_16_avx2_matches_scalar,
                       first);
#if HAVE_AVX512
    first = report_all("test_adm_dwt2_8_avx512_matches_scalar",
                       test_adm_dwt2_8_avx512_matches_scalar, first);
    first = report_all("test_adm_dwt2_16_avx512_matches_scalar",
                       test_adm_dwt2_16_avx512_matches_scalar, first);
#else
    (void)test_adm_dwt2_8_avx512_matches_scalar;
    (void)test_adm_dwt2_16_avx512_matches_scalar;
#endif
#else
    (void)fprintf(stderr, "skipping: non-x86 arch\n");
    (void)test_adm_dwt2_8_avx2_matches_scalar;
    (void)test_adm_dwt2_16_avx2_matches_scalar;
    (void)test_adm_dwt2_8_avx512_matches_scalar;
    (void)test_adm_dwt2_16_avx512_matches_scalar;
#endif
    return first;
}

/* NOLINTEND(modernize-use-nullptr) */
