/**
 *
 *  Copyright 2016-2026 Netflix, Inc.
 *  Copyright 2026 Lusoris
 *  SPDX-License-Identifier: BSD-2-Clause-Patent
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

#include <errno.h>
#include <immintrin.h>
#include <math.h>
#include <stddef.h>
#include "float_adm_avx2.h"
#include "mem.h"

/*
 * AVX2 kernels of the float ADM pipeline: the wavelet (adm_dwt2_s()) and one
 * band of the CSF stage (adm_csf_plane_s()). Both return the scalar
 * function's bits for every input; core/test/test_float_adm_x86.c compares
 * every output element. What that takes:
 *
 *  - a four-tap sum starts at +0 and adds one product per step, in tap order,
 *    as `accum = 0; accum += c[0] * s0; ...` does. Starting at the first
 *    product instead returns -0 where the scalar returns +0. The vector sum
 *    does not spell the first step `_mm256_add_ps(_mm256_setzero_ps(), p)`:
 *    MSVC drops an intrinsic addition of +0 even under /fp:precise (19.51),
 *    so dwt2_plus_zero_avx2() forms `+0 + p` with a compare and a mask;
 *  - multiply, then add. No fused multiply-add: the scalar function carries a
 *    contraction guard (ADR-1057) and this translation unit is built with
 *    the strict floating-point arguments (ADR-1415);
 *  - the CSF's `flt` is a double product narrowed to float, because
 *    FLOAT_ONE_BY_30 is a double constant in adm_tools.c.
 */

static const int FLOAT_ABS_MASK_I = 0x7FFFFFFF;

static const float dwt2_db2_coeffs_lo[4] = {0.482962913144690f, 0.836516303737469f,
                                            0.224143868041857f, -0.129409522550921f};

static const float dwt2_db2_coeffs_hi[4] = {-0.129409522550921f, -0.224143868041857f,
                                            0.836516303737469f, -0.482962913144690f};

/* The broadcast taps of both passes. */
typedef struct Dwt2TapsAvx2 {
    __m256 lo[4];
    __m256 hi[4];
} Dwt2TapsAvx2;

/* One row of the four output bands. */
typedef struct Dwt2BandRow {
    float *a;
    float *v;
    float *h;
    float *d;
} Dwt2BandRow;

/* Four-tap sum of one sample, in adm_dwt2_s()'s order. */
static inline float dwt2_tap4(const float c[4], float s0, float s1, float s2, float s3)
{
    float accum = 0.0f;
    accum += c[0] * s0;
    accum += c[1] * s1;
    accum += c[2] * s2;
    accum += c[3] * s3;
    return accum;
}

/* `+0 + p` for eight samples: -0 becomes +0, every other value (NaN
 * included) stays itself. Written without an addition because MSVC removes
 * `_mm256_add_ps(_mm256_setzero_ps(), p)` as if it were `p` (19.51, /O2
 * /fp:precise), which keeps -0 where the scalar sum returns +0. */
static inline __m256 dwt2_plus_zero_avx2(__m256 p)
{
    return _mm256_and_ps(p, _mm256_cmp_ps(p, _mm256_setzero_ps(), _CMP_NEQ_UQ));
}

/* The same sum for eight samples. */
static inline __m256 dwt2_tap4_avx2(const __m256 c[4], __m256 s0, __m256 s1, __m256 s2, __m256 s3)
{
    __m256 accum = dwt2_plus_zero_avx2(_mm256_mul_ps(c[0], s0));
    accum = _mm256_add_ps(accum, _mm256_mul_ps(c[1], s1));
    accum = _mm256_add_ps(accum, _mm256_mul_ps(c[2], s2));
    accum = _mm256_add_ps(accum, _mm256_mul_ps(c[3], s3));
    return accum;
}

/* Vertical pass of one output row: the four source rows `row[0..3]` filtered
 * into tmplo and tmphi, `w` samples each. */
static void dwt2_vertical_row_avx2(const Dwt2TapsAvx2 *taps, const float *const row[4], int w,
                                   float *tmplo, float *tmphi)
{
    int j = 0;
    for (; j + 8 <= w; j += 8) {
        const __m256 s0 = _mm256_loadu_ps(row[0] + j);
        const __m256 s1 = _mm256_loadu_ps(row[1] + j);
        const __m256 s2 = _mm256_loadu_ps(row[2] + j);
        const __m256 s3 = _mm256_loadu_ps(row[3] + j);

        _mm256_storeu_ps(tmplo + j, dwt2_tap4_avx2(taps->lo, s0, s1, s2, s3));
        _mm256_storeu_ps(tmphi + j, dwt2_tap4_avx2(taps->hi, s0, s1, s2, s3));
    }

    for (; j < w; ++j) {
        const float s0 = row[0][j];
        const float s1 = row[1][j];
        const float s2 = row[2][j];
        const float s3 = row[3][j];

        tmplo[j] = dwt2_tap4(dwt2_db2_coeffs_lo, s0, s1, s2, s3);
        tmphi[j] = dwt2_tap4(dwt2_db2_coeffs_hi, s0, s1, s2, s3);
    }
}

/* Horizontal pass, scalar, for output column j, through ind_x: the left
 * boundary (j = 0, reflection) and the right end of a row. */
static void dwt2_horizontal_scalar(const float *tmplo, const float *tmphi, int **ind_x, int j,
                                   const Dwt2BandRow *out)
{
    const int j0 = ind_x[0][j];
    const int j1 = ind_x[1][j];
    const int j2 = ind_x[2][j];
    const int j3 = ind_x[3][j];

    out->a[j] = dwt2_tap4(dwt2_db2_coeffs_lo, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
    out->v[j] = dwt2_tap4(dwt2_db2_coeffs_hi, tmplo[j0], tmplo[j1], tmplo[j2], tmplo[j3]);
    out->h[j] = dwt2_tap4(dwt2_db2_coeffs_lo, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
    out->d[j] = dwt2_tap4(dwt2_db2_coeffs_hi, tmphi[j0], tmphi[j1], tmphi[j2], tmphi[j3]);
}

/* Even (`odd` = 0) or odd elements of the sixteen floats at `p`. */
static inline __m256 dwt2_every_other_avx2(const float *p, int odd)
{
    const __m256 lo = _mm256_loadu_ps(p);
    const __m256 hi = _mm256_loadu_ps(p + 8);
    /* Per 128-bit lane: two elements of lo, then two of hi. */
    const __m256 mixed = odd ? _mm256_shuffle_ps(lo, hi, 0xDD) : _mm256_shuffle_ps(lo, hi, 0x88);
    /* Lanes back in order: lo's four elements, then hi's. */
    return _mm256_castpd_ps(_mm256_permute4x64_pd(_mm256_castps_pd(mixed), 0xD8));
}

/* Eight outputs of the low and of the high horizontal filter from one
 * temporary row. `taps_at` points at tmp[2j]: the four taps of output j + k
 * are tmp[2(j + k) - 1 .. 2(j + k) + 2], which is what ind_x holds for every
 * column that needs no reflection. */
static inline void dwt2_horizontal_8_avx2(const Dwt2TapsAvx2 *taps, const float *taps_at,
                                          float *out_lo, float *out_hi)
{
    const __m256 tap0 = dwt2_every_other_avx2(taps_at - 1, 0);
    const __m256 tap1 = dwt2_every_other_avx2(taps_at - 1, 1);
    const __m256 tap2 = dwt2_every_other_avx2(taps_at + 1, 0);
    const __m256 tap3 = dwt2_every_other_avx2(taps_at + 1, 1);

    _mm256_storeu_ps(out_lo, dwt2_tap4_avx2(taps->lo, tap0, tap1, tap2, tap3));
    _mm256_storeu_ps(out_hi, dwt2_tap4_avx2(taps->hi, tap0, tap1, tap2, tap3));
}

/* Horizontal pass of one output row: column 0 and the right end through
 * ind_x, the columns between eight at a time. The vector loop stops while
 * its last tap, tmp[2j + 16], is still inside the row. */
static void dwt2_horizontal_row_avx2(const Dwt2TapsAvx2 *taps, const float *tmplo,
                                     const float *tmphi, int **ind_x, int w, const Dwt2BandRow *out)
{
    const int half_w = (w + 1) / 2;

    dwt2_horizontal_scalar(tmplo, tmphi, ind_x, 0, out);

    int j = 1;
    for (; j + 8 <= half_w && 2 * j + 16 < w; j += 8) {
        const ptrdiff_t at = (ptrdiff_t)2 * j;
        dwt2_horizontal_8_avx2(taps, tmplo + at, out->a + j, out->v + j);
        dwt2_horizontal_8_avx2(taps, tmphi + at, out->h + j, out->d + j);
    }

    for (; j < half_w; ++j) {
        dwt2_horizontal_scalar(tmplo, tmphi, ind_x, j, out);
    }
}

int float_adm_dwt2_avx2(const float *src, const adm_dwt_band_t_s *dst, int **ind_y, int **ind_x,
                        int w, int h, int src_stride, int dst_stride)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    float *tmplo = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    float *tmphi = aligned_malloc(ALIGN_CEIL(sizeof(float) * w), MAX_ALIGN);
    if (!tmplo || !tmphi) {
        aligned_free(tmplo);
        aligned_free(tmphi);
        return -ENOMEM;
    }

    const Dwt2TapsAvx2 taps = {
        .lo = {_mm256_set1_ps(dwt2_db2_coeffs_lo[0]), _mm256_set1_ps(dwt2_db2_coeffs_lo[1]),
               _mm256_set1_ps(dwt2_db2_coeffs_lo[2]), _mm256_set1_ps(dwt2_db2_coeffs_lo[3])},
        .hi = {_mm256_set1_ps(dwt2_db2_coeffs_hi[0]), _mm256_set1_ps(dwt2_db2_coeffs_hi[1]),
               _mm256_set1_ps(dwt2_db2_coeffs_hi[2]), _mm256_set1_ps(dwt2_db2_coeffs_hi[3])},
    };

    for (int i = 0; i < (h + 1) / 2; ++i) {
        const float *const row[4] = {
            src + (ptrdiff_t)ind_y[0][i] * src_px_stride,
            src + (ptrdiff_t)ind_y[1][i] * src_px_stride,
            src + (ptrdiff_t)ind_y[2][i] * src_px_stride,
            src + (ptrdiff_t)ind_y[3][i] * src_px_stride,
        };
        const ptrdiff_t at = (ptrdiff_t)i * dst_px_stride;
        const Dwt2BandRow out = {dst->band_a + at, dst->band_v + at, dst->band_h + at,
                                 dst->band_d + at};

        dwt2_vertical_row_avx2(&taps, row, w, tmplo, tmphi);
        dwt2_horizontal_row_avx2(&taps, tmplo, tmphi, ind_x, w, &out);
    }

    aligned_free(tmplo);
    aligned_free(tmphi);
    return 0;
}

/* `one_by_30 * |d|` for eight floats, as a double product narrowed to float. */
static inline __m256 csf_filter_avx2(__m256d one_by_30, __m256 abs_d)
{
    const __m256d lo = _mm256_cvtps_pd(_mm256_castps256_ps128(abs_d));
    const __m256d hi = _mm256_cvtps_pd(_mm256_extractf128_ps(abs_d, 1));
    const __m128 flo = _mm256_cvtpd_ps(_mm256_mul_pd(one_by_30, lo));
    const __m128 fhi = _mm256_cvtpd_ps(_mm256_mul_pd(one_by_30, hi));
    return _mm256_insertf128_ps(_mm256_castps128_ps256(flo), fhi, 1);
}

void float_adm_csf_avx2(const float *src, float *dst, float *flt, int w, int h, int src_stride,
                        int dst_stride, float factor, double one_by_30)
{
    const int src_px_stride = src_stride / sizeof(float);
    const int dst_px_stride = dst_stride / sizeof(float);

    const __m256 abs_mask = _mm256_castsi256_ps(_mm256_set1_epi32(FLOAT_ABS_MASK_I));
    const __m256 vfactor = _mm256_set1_ps(factor);
    const __m256d vone_by_30 = _mm256_set1_pd(one_by_30);

    for (int i = 0; i < h; ++i) {
        const float *src_row = src + (ptrdiff_t)i * src_px_stride;
        float *dst_row = dst + (ptrdiff_t)i * dst_px_stride;
        float *flt_row = flt + (ptrdiff_t)i * dst_px_stride;
        int j = 0;

        for (; j + 8 <= w; j += 8) {
            const __m256 d = _mm256_mul_ps(vfactor, _mm256_loadu_ps(src_row + j));
            _mm256_storeu_ps(dst_row + j, d);
            _mm256_storeu_ps(flt_row + j, csf_filter_avx2(vone_by_30, _mm256_and_ps(d, abs_mask)));
        }

        for (; j < w; ++j) {
            const float dst_val = factor * src_row[j];
            dst_row[j] = dst_val;
            flt_row[j] = (float)(one_by_30 * fabsf(dst_val));
        }
    }
}
